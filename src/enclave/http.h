// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "http_builder.h"
#include "tlsendpoint.h"

#include <http-parser/http_parser.h>

namespace enclave
{
  namespace http
  {
    class MsgProcessor
    {
    public:
      virtual void msg(
        http_method method,
        const std::string& path,
        const std::string& query,
        std::vector<uint8_t> body) = 0;
    };

    enum State
    {
      DONE,
      IN_MESSAGE
    };

    static int on_msg_begin(http_parser* parser);
    static int on_url(http_parser* parser, const char* at, size_t length);
    static int on_req(http_parser* parser, const char* at, size_t length);
    static int on_msg_end(http_parser* parser);

    std::string extract_url_field(
      const http_parser_url& url, http_parser_url_fields field, char const* raw)
    {
      if ((1 << field) & url.field_set)
      {
        const auto& data = url.field_data[field];
        return std::string(raw + data.off, raw + data.off + data.len);
      }

      return {};
    }

    class Parser
    {
    private:
      http_parser parser;
      http_parser_settings settings;
      MsgProcessor& proc;
      State state = DONE;
      std::vector<uint8_t> buf;
      std::string path = "";
      std::string query = "";

    public:
      Parser(http_parser_type type, MsgProcessor& proc_) : proc(proc_)
      {
        http_parser_settings_init(&settings);

        settings.on_message_begin = on_msg_begin;
        settings.on_url = on_url;
        settings.on_body = on_req;
        settings.on_message_complete = on_msg_end;

        http_parser_init(&parser, type);
        parser.data = this;
      }

      size_t execute(const uint8_t* data, size_t size)
      {
        auto parsed =
          http_parser_execute(&parser, &settings, (const char*)data, size);
        LOG_TRACE_FMT("Parsed {} bytes", parsed);
        auto err = HTTP_PARSER_ERRNO(&parser);
        if (err)
        {
          throw std::runtime_error(fmt::format(
            "HTTP parsing failed: {}: {}",
            http_errno_name(err),
            http_errno_description(err)));
        }

        LOG_TRACE_FMT(
          "Parsed a {} request", http_method_str(http_method(parser.method)));

        // TODO: check for http->upgrade to support websockets
        return parsed;
      }

      void append(const char* at, size_t length)
      {
        if (state == IN_MESSAGE)
        {
          LOG_TRACE_FMT("Appending chunk [{}]", std::string_view(at, length));
          std::copy(at, at + length, std::back_inserter(buf));
        }
        else
        {
          throw std::runtime_error("Receiving content outside of message");
        }
      }

      void new_message()
      {
        if (state == DONE)
        {
          LOG_TRACE_FMT("Entering new message");
          state = IN_MESSAGE;
          buf.clear();
        }
        else
        {
          throw std::runtime_error(
            "Entering new message when previous message isn't complete");
        }
      }

      void end_message()
      {
        if (state == IN_MESSAGE)
        {
          LOG_TRACE_FMT("Done with message");
          proc.msg(http_method(parser.method), path, query, std::move(buf));
          state = DONE;
        }
        else
        {
          throw std::runtime_error("Ending message, but not in a message");
        }
      }

      void parse_url(const char* at, size_t length)
      {
        LOG_TRACE_FMT(
          "Received url to parse: {}", std::string_view(at, length));

        http_parser_url url;
        http_parser_url_init(&url);

        const auto err = http_parser_parse_url(at, length, 0, &url);
        if (err != 0)
        {
          throw std::runtime_error(fmt::format("Error parsing url: {}", err));
        }

        path = extract_url_field(url, UF_PATH, at);

        query = extract_url_field(url, UF_QUERY, at);
      }
    };

    class ResponseHeaderEmitter
    {
    public:
      static std::vector<uint8_t> emit(const std::vector<uint8_t>& data)
      {
        if (data.size() == 0)
        {
          auto hdr = fmt::format("HTTP/1.1 204 No Content\r\n");
          return std::vector<uint8_t>(hdr.begin(), hdr.end());
        }
        else
        {
          auto hdr = fmt::format(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: {}\r\n\r\n",
            data.size());
          return std::vector<uint8_t>(hdr.begin(), hdr.end());
        }
      }
    };

    class RequestHeaderEmitter
    {
    public:
      static std::vector<uint8_t> emit(const std::vector<uint8_t>& data)
      {
        return http::build_post_header(data);
      }
    };

    static int on_msg_begin(http_parser* parser)
    {
      Parser* p = reinterpret_cast<Parser*>(parser->data);
      p->new_message();
      return 0;
    }

    static int on_url(http_parser* parser, const char* at, size_t length)
    {
      Parser* p = reinterpret_cast<Parser*>(parser->data);
      p->parse_url(at, length);
      return 0;
    }

    static int on_req(http_parser* parser, const char* at, size_t length)
    {
      Parser* p = reinterpret_cast<Parser*>(parser->data);
      p->append(at, length);
      return 0;
    }

    static int on_msg_end(http_parser* parser)
    {
      Parser* p = reinterpret_cast<Parser*>(parser->data);
      p->end_message();
      return 0;
    }
  }

  template <class E>
  class HTTPEndpoint : public TLSEndpoint, public http::MsgProcessor
  {
  protected:
    http::Parser p;

  public:
    HTTPEndpoint(
      size_t session_id,
      ringbuffer::AbstractWriterFactory& writer_factory,
      std::unique_ptr<tls::Context> ctx) = delete;

    void recv(const uint8_t* data, size_t size)
    {
      recv_buffered(data, size);

      LOG_TRACE_FMT("recv called with {} bytes", size);

      auto buf = read_all_available();

      if (buf.size() == 0)
        return;

      LOG_TRACE_FMT(
        "Going to parse {} bytes: [{}]",
        buf.size(),
        std::string(buf.begin(), buf.end()));

      // TODO: This should return an error to the client if this fails
      if (p.execute(buf.data(), buf.size()) == 0)
      {
        LOG_FAIL_FMT("Failed to parse request");
        return;
      }
    }

    virtual void msg(
      http_method method,
      const std::string& path,
      const std::string& query,
      std::vector<uint8_t> body)
    {
      if (body.size() > 0)
      {
        try
        {
          if (!handle_data(body))
            close();
        }
        catch (...)
        {
          // On any exception, close the connection.
          close();
        }
      }
    }

    void send(const std::vector<uint8_t>& data)
    {
      send_buffered(E::emit(data));
      if (data.size() > 0)
        send_buffered(data);
      flush();
    }
  };

  template <>
  HTTPEndpoint<http::RequestHeaderEmitter>::HTTPEndpoint(
    size_t session_id,
    ringbuffer::AbstractWriterFactory& writer_factory,
    std::unique_ptr<tls::Context> ctx) :
    TLSEndpoint(session_id, writer_factory, std::move(ctx)),
    p(HTTP_RESPONSE, *this)
  {}

  template <>
  HTTPEndpoint<http::ResponseHeaderEmitter>::HTTPEndpoint(
    size_t session_id,
    ringbuffer::AbstractWriterFactory& writer_factory,
    std::unique_ptr<tls::Context> ctx) :
    TLSEndpoint(session_id, writer_factory, std::move(ctx)),
    p(HTTP_REQUEST, *this)
  {}
}