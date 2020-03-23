#include "musicbrainz/web/musicbrainzrecordingstask.h"

#include <QMetaMethod>
#include <QThread>
#include <QXmlStreamReader>

#include "defs_urls.h"
#include "musicbrainz/gzip.h"
#include "musicbrainz/musicbrainzxml.h"
#include "network/httpstatuscode.h"
#include "util/assert.h"
#include "util/compatibility.h"
#include "util/logger.h"
#include "util/version.h"

namespace mixxx {

namespace {

const Logger kLogger("MusicBrainzRecordingsTask");

const QUrl kBaseUrl = QStringLiteral("https://musicbrainz.org/");

const QString kRequestPath = QStringLiteral("/ws/2/recording/");

const QByteArray kUserAgentRawHeaderKey = "User-Agent";

QString userAgentRawHeaderValue() {
    return Version::applicationName() +
            QStringLiteral("/") +
            Version::version() +
            QStringLiteral(" ( ") +
            QStringLiteral(MIXXX_WEBSITE_URL) +
            QStringLiteral(" )");
}

QUrlQuery createUrlQuery() {
    typedef QPair<QString, QString> Param;
    QList<Param> params;
    params << Param("inc", "artists+artist-credits+releases+release-groups+media");

    QUrlQuery query;
    query.setQueryItems(params);
    return query;
}

QNetworkRequest createNetworkRequest(
    const QUuid& recordingId) {
    DEBUG_ASSERT(kBaseUrl.isValid());
    DEBUG_ASSERT(!recordingId.isNull());
    QUrl url = kBaseUrl;
    url.setPath(kRequestPath + uuidToStringWithoutBraces(recordingId));
    url.setQuery(createUrlQuery());
    DEBUG_ASSERT(url.isValid());
    QNetworkRequest networkRequest(url);
    // https://musicbrainz.org/doc/XML_Web_Service/Rate_Limiting#Provide_meaningful_User-Agent_strings
    // HTTP request headers must be latin1.
    networkRequest.setRawHeader(
            kUserAgentRawHeaderKey,
            userAgentRawHeaderValue().toLatin1());
    return networkRequest;
}

} // anonymous namespace

MusicBrainzRecordingsTask::MusicBrainzRecordingsTask(
        QNetworkAccessManager* networkAccessManager,
        QList<QUuid>&& recordingIds)
        : network::WebTask(
                  networkAccessManager),
          m_queuedRecordingIds(std::move(recordingIds)),
          m_parentTimeoutMillis(0) {
    musicbrainz::registerMetaTypesOnce();
}

bool MusicBrainzRecordingsTask::doStart(
        QNetworkAccessManager* networkAccessManager,
        int parentTimeoutMillis) {
    m_parentTimeoutMillis = parentTimeoutMillis;
    DEBUG_ASSERT(thread() == QThread::currentThread());
    DEBUG_ASSERT(networkAccessManager);
    VERIFY_OR_DEBUG_ASSERT(!m_pendingNetworkReply) {
        kLogger.warning()
                << "Task has already been started";
        return false;
    }

    VERIFY_OR_DEBUG_ASSERT(!m_queuedRecordingIds.isEmpty()) {
        kLogger.warning()
                << "Nothing to do";
        return false;
    }
    const auto recordingId = m_queuedRecordingIds.takeFirst();
    DEBUG_ASSERT(!recordingId.isNull());

    const QNetworkRequest networkRequest =
            createNetworkRequest(recordingId);

    if (kLogger.traceEnabled()) {
        kLogger.trace()
                << "GET"
                << networkRequest.url();
    }
    m_pendingNetworkReply =
            networkAccessManager->get(networkRequest);
    VERIFY_OR_DEBUG_ASSERT(m_pendingNetworkReply) {
        kLogger.warning()
                << "Request not sent";
        return false;
    }

    connect(m_pendingNetworkReply,
            &QNetworkReply::finished,
            this,
            &MusicBrainzRecordingsTask::slotNetworkReplyFinished,
            Qt::UniqueConnection);

    connect(m_pendingNetworkReply,
            QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error),
            this,
            &MusicBrainzRecordingsTask::slotNetworkReplyFinished,
            Qt::UniqueConnection);

    return true;
}

void MusicBrainzRecordingsTask::doAbort() {
    if (m_pendingNetworkReply) {
        m_pendingNetworkReply->abort();
    }
}

void MusicBrainzRecordingsTask::slotNetworkReplyFinished() {
    DEBUG_ASSERT(thread() == QThread::currentThread());
    const QPair<QNetworkReply*, network::HttpStatusCode>
            networkReplyWithStatusCode = receiveNetworkReply();
    auto* const networkReply = networkReplyWithStatusCode.first;
    if (!networkReply) {
        // already aborted
        return;
    }
    const auto statusCode = networkReplyWithStatusCode.second;
    VERIFY_OR_DEBUG_ASSERT(networkReply == m_pendingNetworkReply) {
        return;
    }
    m_pendingNetworkReply = nullptr;

    const QByteArray body = networkReply->readAll();
    QXmlStreamReader reader(body);

    // HTTP status of successful results:
    // 200: Found
    // 301: Found, but UUID moved permanently in database
    // 404: Not found in database, i.e. empty result
    if (statusCode != 200 && statusCode != 301 && statusCode != 404) {
        kLogger.info()
                << "GET reply"
                << "statusCode:" << statusCode
                << "body:" << body;
        const auto error = musicbrainz::Error(reader);
        emitFailed(
                network::WebResponse(
                        networkReply->url(),
                        statusCode),
                error.code,
                error.message);
        return;
    }

    auto recordingsResult = musicbrainz::parseRecordings(reader);
    for (auto&& trackRelease : recordingsResult.first) {
        // In case of a response with status 301 (Moved Permanently)
        // the actual recording id might differ from the requested id.
        // To avoid requesting recording ids twice we need to remember
        // all recording ids.
        m_finishedRecordingIds.insert(trackRelease.recordingId);
        m_trackReleases.insert(trackRelease.trackReleaseId, trackRelease);
    }
    if (!recordingsResult.second) {
        kLogger.warning()
                << "Failed to parse XML response";
        emitFailed(
                network::WebResponse(
                        networkReply->url(),
                        statusCode),
                -1,
                "Failed to parse XML response");
        return;
    }

    if (m_queuedRecordingIds.isEmpty()) {
        // Finished all recording ids
        m_finishedRecordingIds.clear();
        auto trackReleases = m_trackReleases.values();
        m_trackReleases.clear();
        emitSucceeded(std::move(trackReleases));
        return;
    }

    // Continue with next recording id
    DEBUG_ASSERT(!m_queuedRecordingIds.isEmpty());
    slotStart(m_parentTimeoutMillis);
}

void MusicBrainzRecordingsTask::emitSucceeded(
        QList<musicbrainz::TrackRelease>&& trackReleases) {
    const auto signal = QMetaMethod::fromSignal(
            &MusicBrainzRecordingsTask::succeeded);
    DEBUG_ASSERT(receivers(signal.name()) <= 1); // unique connection
    if (isSignalConnected(signal)) {
        emit succeeded(
                std::move(trackReleases));
    } else {
        deleteLater();
    }
}
void MusicBrainzRecordingsTask::emitFailed(
        network::WebResponse response,
        int errorCode,
        QString errorMessage) {
    const auto signal = QMetaMethod::fromSignal(
            &MusicBrainzRecordingsTask::succeeded);
    DEBUG_ASSERT(receivers(signal.name()) <= 1); // unique connection
    if (isSignalConnected(signal)) {
        emit failed(
                response,
                errorCode,
                errorMessage);
    } else {
        deleteLater();
    }
}

} // namespace mixxx
