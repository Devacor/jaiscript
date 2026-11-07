#pragma once
// A tls_layer for http_socket_transport.hpp on OpenSSL 1.1.1 or 3.x: TLS 1.2 and up, SNI, the peer's chain verified
// against the context's trust store and its name against the host (SSL_set1_host, an IP address against its
// subjectAltName). The host owns the trust: the constructor's callback fills the store (OpenSSL's default paths when
// none is given). Header-only and opt-in: include it only where OpenSSL is linked.
#include <jaiscript/net/http_socket_transport.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <functional>
#include <memory>
#include <string>

namespace jai {
namespace net {

	class openssl_tls : public tls_layer {
	public:
		using trust_setup = std::function<void(SSL_CTX*)>;

		explicit openssl_tls(trust_setup a_trust = {}) {
			context_ = SSL_CTX_new(TLS_client_method());
			if (!context_) { return; }
			SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
			SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
			if (a_trust) {
				a_trust(context_);
			} else {
				SSL_CTX_set_default_verify_paths(context_);
			}
			ERR_clear_error();
		}
		openssl_tls(const openssl_tls&) = delete;
		openssl_tls& operator=(const openssl_tls&) = delete;
		~openssl_tls() override {
			if (context_) { SSL_CTX_free(context_); }
		}

		std::unique_ptr<byte_stream> secure(socket_stream& a_socket, const std::string& a_host, clock::time_point a_deadline, std::string& a_error) override {
			if (!context_) {
				a_error = "TLS is unavailable: the OpenSSL context did not start";
				return nullptr;
			}
			auto result = std::make_unique<stream>(a_socket, SSL_new(context_));
			SSL* ssl = result->ssl;
			if (!ssl || SSL_set_fd(ssl, static_cast<int>(a_socket.native())) != 1) {
				a_error = "TLS is unavailable: " + last_error();
				return nullptr;
			}
			X509_VERIFY_PARAM* parameters = SSL_get0_param(ssl);
			X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			const bool literal_address = X509_VERIFY_PARAM_set1_ip_asc(parameters, a_host.c_str()) == 1;
			if (!literal_address && (SSL_set_tlsext_host_name(ssl, a_host.c_str()) != 1 || SSL_set1_host(ssl, a_host.c_str()) != 1)) {
				a_error = "TLS cannot verify a peer as '" + a_host + "'";
				return nullptr;
			}
			while (true) {
				const int done = SSL_connect(ssl);
				if (done == 1) { break; }
				const int why = SSL_get_error(ssl, done);
				if (why == SSL_ERROR_WANT_READ || why == SSL_ERROR_WANT_WRITE) {
					const int ready = a_socket.wait(why == SSL_ERROR_WANT_WRITE, a_deadline);
					if (ready == 0) {
						a_error = "timed out";
						return nullptr;
					}
					if (ready > 0) { continue; }
				}
				const long verified = SSL_get_verify_result(ssl);
				a_error = verified != X509_V_OK ? std::string("the TLS certificate of '") + a_host + "' did not verify: " + X509_verify_cert_error_string(verified)
					: "the TLS handshake with '" + a_host + "' failed: " + last_error();
				return nullptr;
			}
			return result;
		}

	private:
		struct stream : byte_stream {
			stream(socket_stream& a_socket, SSL* a_ssl) : socket(a_socket), ssl(a_ssl) {}
			~stream() override {
				if (ssl) { SSL_free(ssl); }
			}

			bool write_all(std::string_view a_bytes, clock::time_point a_deadline, std::string& a_error) override {
				while (!a_bytes.empty()) {
					const int sent = SSL_write(ssl, a_bytes.data(), static_cast<int>(std::min<size_t>(a_bytes.size(), 1 << 30)));
					if (sent > 0) {
						a_bytes.remove_prefix(static_cast<size_t>(sent));
						continue;
					}
					if (!wait_for(SSL_get_error(ssl, sent), a_deadline, a_error, "sending")) { return false; }
				}
				return true;
			}

			long read_some(char* a_data, size_t a_size, clock::time_point a_deadline, std::string& a_error) override {
				while (true) {
					const int got = SSL_read(ssl, a_data, static_cast<int>(std::min<size_t>(a_size, 1 << 30)));
					if (got > 0) { return got; }
					const int why = SSL_get_error(ssl, got);
					if (why == SSL_ERROR_ZERO_RETURN) { return 0; }
					if (why == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) { return 0; }   // a close without close_notify: the framing decides
					if (!wait_for(why, a_deadline, a_error, "receiving")) { return -1; }
				}
			}

			bool wait_for(int a_why, clock::time_point a_deadline, std::string& a_error, const char* a_doing) {
				if (a_why == SSL_ERROR_WANT_READ || a_why == SSL_ERROR_WANT_WRITE) {
					const int ready = socket.wait(a_why == SSL_ERROR_WANT_WRITE, a_deadline);
					if (ready > 0) { return true; }
					a_error = ready == 0 ? "timed out" : std::string("the TLS connection failed while ") + a_doing;
					return false;
				}
				a_error = std::string("the TLS connection failed while ") + a_doing + ": " + last_error();
				return false;
			}

			socket_stream& socket;
			SSL* ssl = nullptr;
		};

		static std::string last_error() {
			const unsigned long code = ERR_get_error();
			ERR_clear_error();
			if (code == 0) { return "no OpenSSL error"; }
			char text[256];
			ERR_error_string_n(code, text, sizeof(text));
			return text;
		}

		SSL_CTX* context_ = nullptr;
	};

} // namespace net
} // namespace jai
