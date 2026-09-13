#include <QCoreApplication>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTimer>

#include <cstdio>
#include <map>

namespace {

using StatusCode = QHttpServerResponse::StatusCode;

const QByteArray kToken = "fovea-dev-token";

QHttpServerResponse jsonError(StatusCode code, const QString &message)
{
    return QHttpServerResponse(QJsonObject{ { "error", message } }, code);
}

bool bearerOk(const QHttpServerRequest &req)
{
    const QByteArray auth = req.headers().value(QHttpHeaders::WellKnownHeader::Authorization).toByteArray();
    return auth.startsWith("Bearer ") && auth.mid(7).trimmed() == kToken;
}

template <typename... Args, typename F>
auto guarded(F fn)
{
    return [fn](Args... args, const QHttpServerRequest &req) -> QHttpServerResponse {
        if (!bearerOk(req))
            return jsonError(StatusCode::Unauthorized, "missing or invalid bearer token");
        return fn(args..., req);
    };
}

std::map<QString, QJsonObject> g_cameras;

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    quint16 port = args.size() > 1 ? quint16(args[1].toUInt()) : 0;
    int seconds = args.size() > 2 ? args[2].toInt() : 20;

    g_cameras["cam1"] = QJsonObject{ { "id", "cam1" }, { "url", "rtsp://127.0.0.1:8554/cam1" } };

    QHttpServer server;

    server.route("/v1/health", QHttpServerRequest::Method::Get, []() {
        return QHttpServerResponse(QJsonObject{ { "ok", true } });
    });

    server.route("/v1/cameras", QHttpServerRequest::Method::Get, guarded<>([](const QHttpServerRequest &) {
        QJsonArray list;
        for (const auto &[id, cam] : g_cameras)
            list.append(cam);
        return QHttpServerResponse(list);
    }));

    server.route("/v1/cameras/<arg>", QHttpServerRequest::Method::Get,
                 guarded<QString>([](const QString &id, const QHttpServerRequest &) {
                     auto it = g_cameras.find(id);
                     if (it == g_cameras.end())
                         return jsonError(StatusCode::NotFound, "no camera " + id);
                     return QHttpServerResponse(it->second);
                 }));

    server.route("/v1/cameras", QHttpServerRequest::Method::Post, guarded<>([](const QHttpServerRequest &req) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            return jsonError(StatusCode::BadRequest, "invalid json: " + err.errorString());
        const QJsonObject cam = doc.object();
        const QString id = cam.value("id").toString();
        if (id.isEmpty())
            return jsonError(StatusCode::BadRequest, "id required");
        g_cameras[id] = cam;
        return QHttpServerResponse(cam, StatusCode::Created);
    }));

    server.route("/v1/cameras/<arg>", QHttpServerRequest::Method::Delete,
                 guarded<QString>([](const QString &id, const QHttpServerRequest &) {
                     if (!g_cameras.erase(id))
                         return jsonError(StatusCode::NotFound, "no camera " + id);
                     return QHttpServerResponse(StatusCode::NoContent);
                 }));

    server.setMissingHandler(&server, [](const QHttpServerRequest &req, QHttpServerResponder &responder) {
        responder.write(QJsonDocument(QJsonObject{ { "error", "no route " + req.url().path() } }),
                        QHttpServerResponder::StatusCode::NotFound);
    });

    server.addAfterRequestHandler(&server, [](const QHttpServerRequest &, QHttpServerResponse &resp) {
        QHttpHeaders h = resp.headers();
        h.append("X-Fovea-Core", "0.1");
        resp.setHeaders(std::move(h));
    });

    auto *tcp = new QTcpServer(&server);
    if (!tcp->listen(QHostAddress::LocalHost, port) || !server.bind(tcp)) {
        std::printf("listen/bind failed: %s\n", qPrintable(tcp->errorString()));
        return 1;
    }
    std::printf("PORT=%u\n", tcp->serverPort());
    std::fflush(stdout);

    QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
