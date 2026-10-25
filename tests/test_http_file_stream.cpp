#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/server/http_adapter.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace Aowis::Hydrology;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    assert(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("mask.u8"));
    // An HTTP provider must work for payloads larger than its read buffer,
    // including embedded zero bytes and partial final chunks.
    std::string expected(1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = char(i % 251);
    {
        QFile result(path);
        assert(result.open(QIODevice::WriteOnly));
        assert(result.write(expected.data(), qint64(expected.size())) == qint64(expected.size()));
    }
    HttpAdapter http;
    http.route("/mask", HttpRequest::Method::Get, [&]()
    {
        auto source = std::make_shared<QFile>(path);
        assert(source->open(QIODevice::ReadOnly));
        return HttpResponse(QByteArrayLiteral("application/vnd.aowis.catchment-u8"), source);
    });
#if defined(Q_OS_UNIX)
    // A download already holding a descriptor must survive eviction's unlink.
    http.route("/unlinked", HttpRequest::Method::Get, [&]()
    {
        auto source = std::make_shared<QFile>(path);
        assert(source->open(QIODevice::ReadOnly));
        assert(QFile::remove(path));
        return HttpResponse(QByteArrayLiteral("application/vnd.aowis.catchment-u8"), source);
    });
#endif
    const int port = http.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread server([&]() { assert(http.listen_after_bind()); });
    for (int i = 0; !http.is_running() && i < 200; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(http.is_running());

    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10);
    auto reply = client.Get("/mask");
    assert(reply && reply->status == 200);
    assert(reply->get_header_value("Content-Type") == "application/vnd.aowis.catchment-u8");
    assert(reply->body == expected);
#if defined(Q_OS_UNIX)
    auto unlinked = client.Get("/unlinked");
    assert(unlinked && unlinked->status == 200 && unlinked->body == expected);
#endif
    http.stop();
    server.join();
    return 0;
}
