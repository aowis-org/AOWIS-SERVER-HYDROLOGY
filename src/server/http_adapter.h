#ifndef AOWIS_HYDROLOGY_HTTP_ADAPTER_H
#define AOWIS_HYDROLOGY_HTTP_ADAPTER_H

// Minimal typed bridge from the service's Qt value types to cpp-httplib.
// There is no Qt HttpServer dependency and no QObjects cross HTTP threads.
#include <httplib.h>
#include "http_status.h"
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace Aowis::Hydrology
{
class HttpRequest
{
public:
    enum class Method { Get, Post, Delete };
    explicit HttpRequest(const httplib::Request &request)
        : content(QByteArray(request.body.data(), qsizetype(request.body.size()))),
          target(QUrl::fromEncoded(QByteArray(request.target.data(),
                                            qsizetype(request.target.size())))) {}
    const QByteArray &body() const { return content; }
    const QUrl &url() const { return target; }
private:
    QByteArray content;
    QUrl target;
};

class HttpResponse
{
public:
    using StatusCode = HttpStatus;
    HttpResponse(QByteArray mime, QByteArray body,
                 StatusCode status = StatusCode::Ok)
        : mime(std::move(mime)), data(std::move(body)), status(int(status)) {}
    void write(httplib::Response &output) const {
        output.status = status;
        if (data.size() > 64 * 1024) {
            // Stream bounded binary products without duplicating the full
            // QByteArray in an additional std::string response buffer.
            auto payload = std::make_shared<QByteArray>(data);
            output.set_content_provider(size_t(payload->size()), mime.toStdString(),
                [payload](size_t offset, size_t length, httplib::DataSink &sink) {
                    if (offset > size_t(payload->size()) ||
                        length > size_t(payload->size()) - offset) return false;
                    return sink.write(payload->constData() + offset, length);
                });
        } else {
            output.set_content(std::string(data.constData(), size_t(data.size())),
                               mime.toStdString());
        }
    }
private:
    QByteArray mime;
    QByteArray data;
    int status;
};

class HttpAdapter final : public httplib::Server
{
public:
    template <typename F>
    void route(const std::string &path, HttpRequest::Method method, F handler)
    {
        std::string target = path;
        size_t position = 0;
        size_t index = 0;
        while ((position = target.find("<arg>", position)) != std::string::npos)
        {
            const auto name = ":arg" + std::to_string(index++);
            target.replace(position, 5, name);
            position += name.length();
        }
        auto bridge = [handler = std::move(handler)](const httplib::Request &request,
                                                    httplib::Response &response) {
            const HttpRequest input(request);
            const auto param = [&](const char *name) {
                const auto it = request.path_params.find(name);
                return it == request.path_params.end()
                    ? QString() : QString::fromUtf8(it->second.data(), int(it->second.size()));
            };
            const QString first = param("arg0");
            const QString second = param("arg1");
            const HttpResponse answer = [&]() -> HttpResponse {
                if constexpr (std::is_invocable_v<F, const QString &, const QString &, const HttpRequest &>)
                    return handler(first, second, input);
                else if constexpr (std::is_invocable_v<F, const QString &, const QString &>)
                    return handler(first, second);
                else if constexpr (std::is_invocable_v<F, const QString &, const HttpRequest &>)
                    return handler(first, input);
                else if constexpr (std::is_invocable_v<F, const QString &>)
                    return handler(first);
                else if constexpr (std::is_invocable_v<F, const HttpRequest &>)
                    return handler(input);
                else
                    return handler();
            }();
            answer.write(response);
        };
        switch (method)
        {
        case HttpRequest::Method::Get: Get(target, std::move(bridge)); break;
        case HttpRequest::Method::Post: Post(target, std::move(bridge)); break;
        case HttpRequest::Method::Delete: Delete(target, std::move(bridge)); break;
        }
    }
};
} // namespace Aowis::Hydrology
#endif
