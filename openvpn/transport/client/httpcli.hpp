//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2013-2014 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// HTTP proxy transport object.

#ifndef OPENVPN_TRANSPORT_CLIENT_HTTPCLI_H
#define OPENVPN_TRANSPORT_CLIENT_HTTPCLI_H

#include <vector>
#include <string>
#include <sstream>
#include <algorithm>                  // for std::min

#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp> // for boost::algorithm::trim_copy

#include <openvpn/common/types.hpp>
#include <openvpn/common/exception.hpp>
#include <openvpn/common/string.hpp>
#include <openvpn/common/base64.hpp>
#include <openvpn/common/split.hpp>
#include <openvpn/common/options.hpp>
#include <openvpn/common/number.hpp>
#include <openvpn/common/userpass.hpp>
#include <openvpn/transport/tcplink.hpp>
#include <openvpn/transport/client/transbase.hpp>
#include <openvpn/transport/socket_protect.hpp>
#include <openvpn/transport/protocol.hpp>
#include <openvpn/http/reply.hpp>
#include <openvpn/proxy/proxyauth.hpp>
#include <openvpn/proxy/httpdigest.hpp>
#include <openvpn/proxy/ntlm.hpp>
#include <openvpn/client/remotelist.hpp>

namespace openvpn {
  namespace HTTPProxyTransport {

    class Options : public RC<thread_safe_refcount>
    {
    public:
      struct CustomHeader : public RC<thread_unsafe_refcount>
      {
	typedef boost::intrusive_ptr<CustomHeader> Ptr;

	std::string p1;
	std::string p2;
      };

      struct CustomHeaderList : public std::vector<CustomHeader::Ptr>
      {
      };

      typedef boost::intrusive_ptr<Options> Ptr;

      Options() : allow_cleartext_auth(false) {}

      RemoteList::Ptr proxy_server;
      std::string username;
      std::string password;
      bool allow_cleartext_auth;

      std::string http_version;
      std::string user_agent;

      CustomHeaderList headers;

      void set_proxy_server(const std::string& host, const std::string& port)
      {
	proxy_server.reset(new RemoteList(host, port, Protocol(Protocol::TCP), "http proxy port"));
      }

      void proxy_server_set_enable_cache(const bool enable_cache)
      {
	proxy_server->set_enable_cache(enable_cache);
      }

      static Ptr parse(const OptionList& opt)
      {
	if (opt.exists("http-proxy"))
	  {
	    Ptr obj(new Options);
	    if (obj->parse_options(opt))
	      return obj;
	  }
	return Ptr();
      }

    private:
      bool parse_options(const OptionList& opt)
      {
	const Option* hp = opt.get_ptr("http-proxy");
	if (hp)
	  {
	    // get server/port
	    set_proxy_server(hp->get(1, 256), hp->get(2, 16));

	    // get creds
	    {
	      std::vector<std::string> user_pass;
	      if (parse_user_pass(opt, "http-proxy-user-pass", &user_pass))
		{
		  if (user_pass.size() >= 1)
		    username = user_pass[0];
		  if (user_pass.size() >= 2)
		    password = user_pass[1];
		}
	    }

	    // allow cleartext auth?
	    allow_cleartext_auth = (hp->get_optional(3, 16) != "auto-nct");

	    // get options
	    const OptionList::IndexList* hpo = opt.get_index_ptr("http-proxy-option");
	    if (hpo)
	      {
		for (OptionList::IndexList::const_iterator i = hpo->begin(); i != hpo->end(); ++i)
		  {
		    const Option& o = opt[*i];
		    const std::string& type = o.get(1, 64);
		    if (type == "VERSION")
		      {
			http_version = o.get(2, 16);
			o.touch();
		      }
		    else if (type == "AGENT")
		      {
			user_agent = o.get(2, 256);
			o.touch();
		      }
		    else if (type == "EXT1" || type == "EXT2" || type == "CUSTOM-HEADER")
		      {
			CustomHeader::Ptr h(new CustomHeader());
			h->p1 = o.get(2, 512);
			h->p2 = o.get_optional(3, 512);
			headers.push_back(h);
			o.touch();
		      }
		  }
	      }
	    return true;
	  }
	else
	  return false;
      }
    };

    // We need access to RAND_API and CRYPTO_API implementations, because proxy
    // authentication methods tend to require crypto and random functionality.
    template <typename RAND_API, typename CRYPTO_API>
    class ClientConfig : public TransportClientFactory
    {
    public:
      typedef boost::intrusive_ptr<ClientConfig> Ptr;

      RemoteList::Ptr remote_list;
      size_t send_queue_max_size;
      size_t free_list_max_size;
      Frame::Ptr frame;
      SessionStats::Ptr stats;

      Options::Ptr http_proxy_options;

      typename RAND_API::Ptr rng; // random data source

      SocketProtect* socket_protect;

      static Ptr new_obj()
      {
	return new ClientConfig;
      }

      virtual TransportClient::Ptr new_client_obj(boost::asio::io_service& io_service,
						  TransportClientParent& parent);

    private:
      ClientConfig()
	: send_queue_max_size(1024),
	  free_list_max_size(8),
	  socket_protect(NULL)
      {}
    };

    template <typename RAND_API, typename CRYPTO_API>
    class Client : public TransportClient
    {
      friend class ClientConfig<RAND_API, CRYPTO_API>;  // calls constructor
      friend class TCPTransport::Link<Client*, false>;  // calls tcp_read_handler

      typedef TCPTransport::Link<Client*, false> LinkImpl;

      typedef AsioDispatchResolve<Client,
				  void (Client::*)(const boost::system::error_code&, boost::asio::ip::tcp::resolver::iterator),
				  boost::asio::ip::tcp::resolver::iterator> AsioDispatchResolveTCP;

      enum {
	Connected=200,
	Forbidden=403,
	NotFound=404,
	ProxyAuthenticationRequired=407,
	ProxyError=502,
	ServiceUnavailable=503,
      };

    public:
      virtual void start()
      {
	if (!impl)
	  {
	    if (!config->http_proxy_options)
	      {
		parent.proxy_error(Error::PROXY_ERROR, "http_proxy_options not defined");
		return;
	      }

	    halt = false;

	    // Get target server host:port.  We don't care about resolving it
	    // since proxy server will do that for us.
	    remote_list().endpoint_available(&server_host, &server_port, NULL);

	    // Get proxy server host:port, and resolve it if not already cached
	    if (proxy_remote_list().endpoint_available(&proxy_host, &proxy_port, NULL))
	      {
		// already cached
		start_connect_();
	      }
	    else
	      {
		// resolve it
		boost::asio::ip::tcp::resolver::query query(proxy_host,
							    proxy_port);
		parent.transport_pre_resolve();
		resolver.async_resolve(query, AsioDispatchResolveTCP(&Client::do_resolve_, this));
	      }
	  }
      }

      virtual bool transport_send_const(const Buffer& buf)
      {
	return send_const(buf);
      }

      virtual bool transport_send(BufferAllocated& buf)
      {
	return send(buf);
      }

      virtual void server_endpoint_info(std::string& host, std::string& port, std::string& proto, std::string& ip_addr) const
      {
	host = server_host;
	port = server_port;
	const IP::Addr addr = server_endpoint_addr();
	proto = "TCP";
	proto += addr.version_string();
	proto += "-via-HTTP";
	ip_addr = addr.to_string();
      }

      virtual IP::Addr server_endpoint_addr() const
      {
	return IP::Addr::from_asio(server_endpoint.address());
      }

      virtual void stop() { stop_(); }
      virtual ~Client() { stop_(); }

    private:
      class ProxyResponseLimit
      {
      public:
	enum {
	  MaxLines=1024,
	  MaxBytes=65536,
	};

	ProxyResponseLimit()
	{
	  reset();
	}

	void reset()
	{
	  n_bytes = n_lines = 0;
	}

	void add(const Buffer& buf)
	{
	  size_t size = buf.size();
	  if ((n_bytes += size) > MaxBytes)
	    OPENVPN_THROW_EXCEPTION("HTTP proxy response too large (> " << MaxBytes << " bytes)");
	  const unsigned char *p = buf.c_data();
	  while (size--)
	    {
	      const unsigned char c = *p++;
	      if (c == '\n')
		{
		  if (++n_lines > MaxLines)
		    OPENVPN_THROW_EXCEPTION("HTTP proxy response too large (> " << MaxLines << " lines)");
		}
	    }
	}

      private:
	size_t n_bytes;
	size_t n_lines;
      };

      Client(boost::asio::io_service& io_service_arg,
	     ClientConfig<RAND_API, CRYPTO_API>* config_arg,
	     TransportClientParent& parent_arg)
	:  io_service(io_service_arg),
	   socket(io_service_arg),
	   config(config_arg),
	   parent(parent_arg),
	   resolver(io_service_arg),
	   halt(false),
	   n_transactions(0),
	   proxy_established(false),
	   http_reply_status(HTTP::ReplyParser::pending),
	   ntlm_phase_2_response_pending(false),
	   drain_content_length(0)
      {
      }

      bool send_const(const Buffer& cbuf)
      {
	if (impl)
	  {
	    BufferAllocated buf(cbuf, 0);
	    return impl->send(buf);
	  }
	else
	  return false;
      }

      bool send(BufferAllocated& buf)
      {
	if (impl)
	  return impl->send(buf);
	else
	  return false;
      }

      void tcp_error_handler(const char *error) // called by LinkImpl and internally
      {
	std::ostringstream os;
	os << "Transport error on '" << server_host << "' via HTTP proxy " << proxy_host << ':' << proxy_port << " : " << error;
	stop();
	parent.transport_error(Error::UNDEF, os.str());
      }

      void proxy_error(const Error::Type fatal_err, const std::string& what)
      {
	std::ostringstream os;
	os << "on " << proxy_host << ':' << proxy_port << ": " << what;
	stop();
	parent.proxy_error(fatal_err, os.str());
      }

      void tcp_read_handler(BufferAllocated& buf) // called by LinkImpl
      {
	if (proxy_established)
	  parent.transport_recv(buf);
	else
	  {
	    try {
	      proxy_read_handler(buf);
	    }
	    catch (const std::exception& e)
	      {
		proxy_error(Error::PROXY_ERROR, e.what());
	      }
	  }
      }

      void tcp_eof_handler() // called by LinkImpl
      {
	if (proxy_established)
	  {
	    config->stats->error(Error::NETWORK_EOF_ERROR);
	    tcp_error_handler("NETWORK_EOF_ERROR");
	  }
	else
	  {
	    try {
	      proxy_eof_handler();
	    }
	    catch (const std::exception& e)
	      {
		proxy_error(Error::PROXY_ERROR, e.what());
	      }
	  }
      }

      void proxy_read_handler(BufferAllocated& buf)
      {
	// for anti-DoS, only allow a maximum number of chars in HTTP response
	proxy_response_limit.add(buf);

	if (http_reply_status == HTTP::ReplyParser::pending)
	  {
	    OPENVPN_LOG_NTNL("FROM PROXY: " << buf.to_string());
	    for (size_t i = 0; i < buf.size(); ++i)
	      {
		http_reply_status = http_parser.consume(http_reply, (char)buf[i]);
		if (http_reply_status != HTTP::ReplyParser::pending)
		  {
		    buf.advance(i+1);
		    if (http_reply_status == HTTP::ReplyParser::success)
		      {
			//OPENVPN_LOG("*** HTTP header parse complete, resid_size=" << buf.size());
			//OPENVPN_LOG(http_reply.to_string());
			    
			// we are connected, switch socket to tunnel mode
			if (http_reply.status_code == Connected)
			  {
			    // switch socket from HTTP proxy handshake mode to OpenVPN protocol mode
			    proxy_established = true;
			    impl->set_raw_mode(false);
			    impl->inject(buf);
			    parent.transport_connecting();
			  }
			else if (ntlm_phase_2_response_pending)
			  ntlm_auth_phase_2_pre();
		      }
		    else
		      {
			throw Exception("HTTP proxy header parse error");
		      }
		    break;
		  }
	      }
	  }

	// handle draining of content
	if (drain_content_length)
	  {
	    const size_t drain = std::min(drain_content_length, buf.size());
	    buf.advance(drain);
	    drain_content_length -= drain;
	    if (!drain_content_length)
	      {
		if (ntlm_phase_2_response_pending)
		  ntlm_auth_phase_2();
	      }
	  }
      }

      HTTPProxy::ProxyAuthenticate::Ptr get_proxy_authenticate_header(const char *type)
      {
	for (HTTP::HeaderList::const_iterator i = http_reply.headers.begin(); i != http_reply.headers.end(); ++i)
	  {
	    const HTTP::Header& h = *i;
	    if (string::strcasecmp(h.name, "proxy-authenticate") == 0)
	      {
		HTTPProxy::ProxyAuthenticate::Ptr pa = new HTTPProxy::ProxyAuthenticate(h.value);
		if (string::strcasecmp(type, pa->method) == 0)
		  return pa;
	      }
	  }
	return HTTPProxy::ProxyAuthenticate::Ptr();
      }

      void proxy_eof_handler()
      {
	if (http_reply_status == HTTP::ReplyParser::success)
	  {
	    if (http_reply.status_code == ProxyAuthenticationRequired)
	      {
		if (n_transactions <= 1)
		  {
		    //OPENVPN_LOG("*** PROXY AUTHENTICATION REQUIRED");

		    if (config->http_proxy_options->username.empty())
		      {
			proxy_error(Error::PROXY_NEED_CREDS, "HTTP proxy requires credentials");
			return;
		      }

		    HTTPProxy::ProxyAuthenticate::Ptr pa;

		    // NTLM
		    pa = get_proxy_authenticate_header("ntlm");
		    if (pa)
		      {
			ntlm_auth_phase_1(*pa);
			return;
		      }

		    // Digest
		    pa = get_proxy_authenticate_header("digest");
		    if (pa)
		      {
			digest_auth(*pa);
			return;
		      }

		    // Basic
		    pa = get_proxy_authenticate_header("basic");
		    if (pa)
		      {
			if (config->http_proxy_options->allow_cleartext_auth)
			  {
			    basic_auth(*pa);
			    return;
			  }
			else
			  throw Exception("HTTP proxy Basic authentication not allowed by user preference");
		      }
		    throw Exception("HTTP proxy-authenticate method must be Basic, Digest, or NTLM");
		  }
		else
		  {
		    proxy_error(Error::PROXY_NEED_CREDS, "HTTP proxy credentials were not accepted");
		    return;
		  }
	      }
	    else if (http_reply.status_code == ProxyError
		     || http_reply.status_code == NotFound
		     || http_reply.status_code == ServiceUnavailable)
	      {
		// this is a nonfatal error, so we pass Error::UNDEF to tell the upper layer to
		// retry the connection
		proxy_error(Error::UNDEF, "HTTP proxy server could not connect to OpenVPN server");
		return;
	      }
	    else if (http_reply.status_code == Forbidden)
	      OPENVPN_THROW_EXCEPTION("HTTP proxy returned Forbidden status code");
	    else
	      OPENVPN_THROW_EXCEPTION("HTTP proxy status code: " << http_reply.status_code);
	  }
	else if (http_reply_status == HTTP::ReplyParser::pending)
	  throw Exception("HTTP proxy unexpected EOF: reply incomplete");
	else
	  throw Exception("HTTP proxy general error");
      }

      void basic_auth(HTTPProxy::ProxyAuthenticate& pa)
      {
	OPENVPN_LOG("Proxy method: Basic" << std::endl << pa.to_string());

	std::ostringstream os;
	gen_headers(os);
	os << "Proxy-Authorization: Basic "
	   << base64->encode(config->http_proxy_options->username + ':' + config->http_proxy_options->password)
	   << "\r\n";
	http_request = os.str();
	reset();
	start_connect_();
      }

      void digest_auth(HTTPProxy::ProxyAuthenticate& pa)
      {
	try {
	  OPENVPN_LOG("Proxy method: Digest" << std::endl << pa.to_string());

	  // constants
	  const std::string http_method = "CONNECT";
	  const std::string nonce_count = "00000001";
	  const std::string qop = "auth";

	  // get values from Proxy-Authenticate header
	  const std::string realm = pa.parms.get_value("realm");
	  const std::string nonce = pa.parms.get_value("nonce");
	  const std::string algorithm = pa.parms.get_value("algorithm");
	  const std::string opaque = pa.parms.get_value("opaque");

	  // generate a client nonce
	  unsigned char cnonce_raw[8];
	  config->rng->rand_bytes(cnonce_raw, sizeof(cnonce_raw));
	  const std::string cnonce = render_hex(cnonce_raw, sizeof(cnonce_raw));

	  // build URI
	  const std::string uri = server_host + ":" + server_port;

	  // calculate session key
	  const std::string session_key = HTTPProxy::Digest<CRYPTO_API>::calcHA1(
	      algorithm,
	      config->http_proxy_options->username,
	      realm,
	      config->http_proxy_options->password,
	      nonce,
	      cnonce);

	  // calculate response
	  const std::string response = HTTPProxy::Digest<CRYPTO_API>::calcResponse(
	      session_key,
	      nonce,
	      nonce_count,
	      cnonce,
	      qop,
	      http_method,
	      uri,
	      "");

	  // generate proxy request
	  std::ostringstream os;
	  gen_headers(os);
	  os << "Proxy-Authorization: Digest username=\"" << config->http_proxy_options->username << "\", realm=\"" << realm << "\", nonce=\"" << nonce << "\", uri=\"" << uri << "\", qop=" << qop << ", nc=" << nonce_count << ", cnonce=\"" << cnonce << "\", response=\"" << response << "\"";
	  if (!opaque.empty())
	    os << ", opaque=\"" + opaque + "\"";
	  os << "\r\n";

	  http_request = os.str();
	  reset();
	  start_connect_();
	}
	catch (const std::exception& e)
	  {
	    proxy_error(Error::PROXY_NEED_CREDS, std::string("Digest Auth: ") + e.what());
	  }
      }

      std::string get_ntlm_phase_2_response()
      {
	for (HTTP::HeaderList::const_iterator i = http_reply.headers.begin(); i != http_reply.headers.end(); ++i)
	  {
	    const HTTP::Header& h = *i;
	    if (string::strcasecmp(h.name, "proxy-authenticate") == 0)
	      {
		std::vector<std::string> v = Split::by_space<std::vector<std::string>, StandardLex, SpaceMatch, Split::NullLimit>(h.value);
		if (v.size() >= 2 && string::strcasecmp("ntlm", v[0]) == 0)
		  return v[1];
	      }
	  }
	return "";
      }

      void ntlm_auth_phase_1(HTTPProxy::ProxyAuthenticate& pa)
      {
	OPENVPN_LOG("Proxy method: NTLM" << std::endl << pa.to_string());

	const std::string phase_1_reply = HTTPProxy::NTLM<RAND_API, CRYPTO_API>::phase_1();

	std::ostringstream os;
	gen_headers(os);
	os << "Proxy-Connection: Keep-Alive\r\n";
	os << "Proxy-Authorization: NTLM " << phase_1_reply << "\r\n";

	http_request = os.str();
	reset();
	ntlm_phase_2_response_pending = true;
	start_connect_();
      }

      void ntlm_auth_phase_2_pre()
      {
	// if content exists, drain it first, then progress to ntlm_auth_phase_2
	const std::string content_length_str = boost::algorithm::trim_copy(http_reply.headers.get_value("content-length"));
	const unsigned int content_length = parse_number_throw<unsigned int>(content_length_str, "content-length");
	if (content_length)
	  drain_content_length = content_length;
	else
	  ntlm_auth_phase_2();
      }

      void ntlm_auth_phase_2()
      {
	ntlm_phase_2_response_pending = false;

	if (http_reply.status_code != ProxyAuthenticationRequired)
	  throw Exception("NTLM phase-2 status is not ProxyAuthenticationRequired");

	const std::string phase_2_response = get_ntlm_phase_2_response();
	if (!phase_2_response.empty())
	  ntlm_auth_phase_3(phase_2_response);
	else
	  throw Exception("NTLM phase-2 response missing");
      }

      void ntlm_auth_phase_3(const std::string& phase_2_response)
      {
	// do the NTLMv2 handshake
	try {
	  //OPENVPN_LOG("NTLM phase 3: " << phase_2_response);

	  const std::string phase_3_reply = HTTPProxy::NTLM<RAND_API, CRYPTO_API>::phase_3(
	      phase_2_response,
	      config->http_proxy_options->username,
	      config->http_proxy_options->password,
	      *config->rng);

	  std::ostringstream os;
	  gen_headers(os);
	  os << "Proxy-Connection: Keep-Alive\r\n";
	  os << "Proxy-Authorization: NTLM " << phase_3_reply << "\r\n";

	  http_request = os.str();
	  reset_partial();
	  http_proxy_send();
	}
	catch (const std::exception& e)
	  {
	    proxy_error(Error::PROXY_NEED_CREDS, std::string("NTLM Auth: ") + e.what());
	  }
      }

      void gen_headers(std::ostringstream& os)
      {
	bool host_header_sent = false;

	// emit custom headers
	{
	  const Options::CustomHeaderList& headers = config->http_proxy_options->headers;
	  for (Options::CustomHeaderList::const_iterator i = headers.begin(); i != headers.end(); ++i)
	    {
	      const Options::CustomHeader& h = **i;
	      if (!h.p2.empty())
		{
		  os << h.p1 << ": " << h.p2 << "\r\n";
		  if (!string::strcasecmp(h.p1, "host"))
		    host_header_sent = true;
		}
	      else
		{
		  os << h.p1 << "\r\n";
		  const std::string h5 = h.p1.substr(0, 5);
		  if (!string::strcasecmp(h5, "host:"))
		    host_header_sent = true;
		}
	    }
	}

	// emit user-agent header
	{
	  const std::string& user_agent = config->http_proxy_options->user_agent;
	  if (!user_agent.empty())
	    os << "User-Agent: " << user_agent << "\r\n";
	}

	// emit host header
	if (!host_header_sent)
	  os << "Host: " << server_host << "\r\n";
      }

      void stop_()
      {
	if (!halt)
	  {
	    halt = true;
	    if (impl)
	      impl->stop();

	    socket.close();
	    resolver.cancel();
	  }
      }

      // do DNS resolve
      void do_resolve_(const boost::system::error_code& error,
		       boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
      {
	if (!halt)
	  {
	    if (!error)
	      {
		// save resolved endpoint list in proxy remote_list
		proxy_remote_list().set_endpoint_list(endpoint_iterator);
		start_connect_();
	      }
	    else
	      {
		std::ostringstream os;
		os << "DNS resolve error on '" << proxy_host << "' for TCP (HTTP proxy): " << error.message();
		config->stats->error(Error::RESOLVE_ERROR);
		stop();
		parent.transport_error(Error::UNDEF, os.str());
	      }
	  }
      }

      void reset()
      {
	stop();
	halt = false;
	proxy_response_limit.reset();
	proxy_established = false;
	reset_partial();
      }

      void reset_partial()
      {
	http_reply_status = HTTP::ReplyParser::pending;
	http_reply.reset();
	http_parser.reset();
	ntlm_phase_2_response_pending = false;
	drain_content_length = 0;
      }

      // do TCP connect
      void start_connect_()
      {
	proxy_remote_list().get_endpoint(server_endpoint);
	OPENVPN_LOG("Contacting " << server_endpoint << " via HTTP Proxy");
	parent.transport_wait_proxy();
	parent.ip_hole_punch(server_endpoint_addr());
	socket.open(server_endpoint.protocol());
#ifdef OPENVPN_PLATFORM_TYPE_UNIX
	if (config->socket_protect)
	  {
	    if (!config->socket_protect->socket_protect(socket.native_handle()))
	      {
		config->stats->error(Error::SOCKET_PROTECT_ERROR);
		stop();
		parent.transport_error(Error::UNDEF, "socket_protect error (HTTP Proxy)");
		return;
	      }
	  }
#endif
	socket.set_option(boost::asio::ip::tcp::no_delay(true));
	socket.async_connect(server_endpoint, asio_dispatch_connect(&Client::start_impl_, this));
      }

      // start I/O on TCP socket
      void start_impl_(const boost::system::error_code& error)
      {
	if (!halt)
	  {
	    if (!error)
	      {
		parent.transport_wait();
		impl.reset(new LinkImpl(this,
					socket,
					config->send_queue_max_size,
					config->free_list_max_size,
					(*config->frame)[Frame::READ_LINK_TCP],
					config->stats));
		impl->set_raw_mode(true);
		impl->start();
		++n_transactions;

		// tell proxy to connect through to OpenVPN server
		http_proxy_send();
	      }
	    else
	      {
		proxy_remote_list().next();

		std::ostringstream os;
		os << "TCP connect error on '" << proxy_host << ':' << proxy_port << "' (" << server_endpoint << ") for TCP-via-HTTP-proxy session: " << error.message();
		config->stats->error(Error::TCP_CONNECT_ERROR);
		stop();
		parent.transport_error(Error::UNDEF, os.str());
	      }
	  }
      }

      void http_proxy_send()
      {
	BufferAllocated buf;
	create_http_connect_msg(buf);
	send(buf);
      }

      // create HTTP CONNECT message
      void create_http_connect_msg(BufferAllocated& buf)
      {
	std::ostringstream os;
	const std::string& http_version = config->http_proxy_options->http_version;
	os << "CONNECT " << server_host << ':' << server_port << " HTTP/";
	if (!http_version.empty())
	  os << http_version;
	else
	  os << "1.0";
	os << "\r\n";
	if (!http_request.empty())
	  os << http_request;
	else
	  gen_headers(os);
	os << "\r\n";
	const std::string str = os.str();
	http_request = "";

	OPENVPN_LOG_NTNL("TO PROXY: " << str);

	config->frame->prepare(Frame::WRITE_HTTP_PROXY, buf);
	buf.write((const unsigned char *)str.c_str(), str.length());
      }

      RemoteList& remote_list() const { return *config->remote_list; }
      RemoteList& proxy_remote_list() const { return *config->http_proxy_options->proxy_server; }

      std::string proxy_host;
      std::string proxy_port;

      std::string server_host;
      std::string server_port;

      boost::asio::io_service& io_service;
      boost::asio::ip::tcp::socket socket;
      typename ClientConfig<RAND_API, CRYPTO_API>::Ptr config;
      TransportClientParent& parent;
      typename LinkImpl::Ptr impl;
      boost::asio::ip::tcp::resolver resolver;
      TCPTransport::Endpoint server_endpoint;
      bool halt;

      unsigned int n_transactions;
      ProxyResponseLimit proxy_response_limit;
      bool proxy_established;
      HTTP::ReplyParser::status http_reply_status;
      HTTP::Reply http_reply;
      HTTP::ReplyParser http_parser;
      std::string http_request;

      bool ntlm_phase_2_response_pending;
      size_t drain_content_length;
    };

    template <typename RAND_API, typename CRYPTO_API>
    inline TransportClient::Ptr ClientConfig<RAND_API, CRYPTO_API>::new_client_obj(boost::asio::io_service& io_service, TransportClientParent& parent)
    {
      return TransportClient::Ptr(new Client<RAND_API, CRYPTO_API>(io_service, this, parent));
    }
  }
} // namespace openvpn

#endif