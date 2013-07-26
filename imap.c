#define _GNU_SOURCE
#include "imap.h"
#include "ldap.h"
#include "config.h"
#include "module.h"
#include "logging.h"
#include "chaining.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/listener.h>
#include <ldap.h>

static int imap_driver_install(struct bufferevent *, struct imap_driver *);

static int parse_request(struct imap_request *, struct evbuffer *, ssize_t);
static void request_free(struct imap_request *);

static void trigger_listener(int, void *);
static void listen_cb(struct evconnlistener *, evutil_socket_t, struct sockaddr *, int socklen, void *);
static void conn_readcb(struct bufferevent *, void *);
static void conn_eventcb(struct bufferevent *, short, void *);

static int imap_capability(struct imap_context *ctx, struct imap_request *req, void *priv);
static int imap_starttls(struct imap_context *ctx, struct imap_request *req, void *priv);
static int imap_login(struct imap_context *ctx, struct imap_request *req, void *priv);

static void search_cb(LDAP *, LDAPMessage *, void *);

static void proxy_cb(struct bufferevent *, void *);
static void server_connect_cb(struct bufferevent *, short, void *);

static struct imap_handler handlers[] = {
    { "STARTTLS", imap_starttls }, // will not be registered by default
    { "CAPABILITY", imap_capability },
    { "LOGIN", imap_login },
    /*
    { "NOOP", imap_noop },
    { "LOGOUT", imap_logout },
    { "AUTHENTICATE", imap_authenticate },
    */
    { NULL }
};

static struct imap_config config_default = {
    .listen = "127.0.0.1:1143",
    .default_host = "localhost",
    .default_port = 143
};

struct module imap_module = {
    .name = "imap",
    .conf = imap_driver_config,
    .init = imap_driver_init,
};

int
imap_handler_cmp(const void *left, const void *right)
{
    const struct imap_handler *l = left;
    const struct imap_handler *r = right;
    return strcasecmp(l->command, r->command);
}

int
imap_driver_config(struct module *module, config_setting_t *conf)
{
    config_setting_t *setting, *value;
    struct imap_config *config;
    struct imap_driver *driver;
    int port;

    if (conf == NULL)
        return 1;

    config = malloc(sizeof(struct imap_config));
    if (config == NULL)
        return 1;
    *config = config_default;

    setting = config_setting_get_member(conf, "listen");
    if (setting) {
        /* in the future there are going to be more listen addresses but not
         * right now */
        value = config_setting_get_elem(setting, 0);
        if (value == NULL)
            return 1;

        conf_get_string(config->listen, value);
    }

    /*TODO: prime for a rewrite */
    setting = config_setting_get_member(conf, "defaults");
    if (setting) {

        value = config_setting_get_elem(setting, 0);
        if (value == NULL)
            return 1;

        conf_get_string(config->default_host, value);

        value = config_setting_get_elem(setting, 1);
        if (value == NULL)
            return 1;

        port = config_setting_get_int(value);
        if ((port > 0) && (port <= 65535)) {
            config->default_port = port;
        } else {
            return 1;
        }
    }

    setting = config_setting_get_member(conf, "tls");
    if (setting != NULL) {
        value = config_setting_get_elem(setting, 0);
        if (value == NULL)
            return 1;

        conf_get_string(config->cert, value);

        value = config_setting_get_elem(setting, 1);
        if (value == NULL)
            return 1;

        conf_get_string(config->pkey, value);
    }

    driver = calloc(1, sizeof(struct imap_driver));
    if (driver == NULL)
        return 1;

    driver->config = config;
    module->priv = driver;

    return 0;
}

int
imap_driver_init(struct module *module, struct event_base *base)
{
    struct imap_driver *driver = module->priv;
    struct imap_config *config;
    struct imap_handler *handler = handlers;
    struct module *ldap;
    struct sockaddr_in6 sin;
    int socklen = sizeof(sin);

    assert(driver && driver->config && base);
    config = driver->config;

    driver->base = base;
    driver->dnsbase = get_dnsbase();

    if (config->cert) {
        driver->ssl_ctx = new_ssl_ctx(config->cert, config->pkey);
        if (driver->ssl_ctx == NULL) {
            return 1;
        }
    } else {
        /* skip the STARTTLS handler */
        handler += 1;
    }

    for (; handler->command; handler++) {
        /* handle the private handler storage */
        if (avl_insert(&driver->commands, handler, imap_handler_cmp, avl_dup_error)) {
            return 1;
        }
    }

    if (evutil_parse_sockaddr_port(config->listen, (struct sockaddr *)&sin, &socklen)) {
        return 1;
    }

    /* we start in disabled state until the LDAP interface is ready */
    driver->listener = evconnlistener_new_bind(base, listen_cb, (void*)driver,
            LEV_OPT_REUSEABLE|LEV_OPT_DISABLED|LEV_OPT_CLOSE_ON_FREE,
            -1, (struct sockaddr *)&sin, socklen);
    /* could also be wise to set an error callback, but what errors do we face
     * on accept()? */

    if (!driver->listener) {
        skeeter_log(LOG_CRIT, "Could not create a listener!");
        return 1;
    }

    ldap = get_module("ldap");
    if (!ldap || !ldap->register_event) {
        skeeter_log(LOG_CRIT, "LDAP module not available!");
        return 1;
    }

    if (ldap->register_event(ldap, MODULE_ANY | MODULE_PERSIST, trigger_listener, driver->listener)) {
        skeeter_log(LOG_CRIT, "Regitration with LDAP module failed!");
        return 1;
    }

    driver->ldap = ldap;

    return 0;
}

static void
trigger_listener(int flags, void *ctx)
{
    struct evconnlistener *listener = ctx;
    if (flags & MODULE_READY)
        evconnlistener_enable(listener);
    else
        evconnlistener_disable(listener);
}

static void
listen_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *sa, int socklen, void *user_data)
{
    struct imap_driver *driver = user_data;
    struct event_base *base = driver->base;
    struct bufferevent* bev;

    bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        skeeter_log(LOG_CRIT, "Could not acquire bufferevent!");
        event_base_loopbreak(base);
        return;
    }

    bufferevent_write(bev, "* CAPABILITY IMAP4rev1 STARTTLS" CRLF, 33);
    skeeter_log(LOG_INFO, "A new connection established");
    imap_driver_install(bev, driver);
}

static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
    if (events & BEV_EVENT_EOF) {
        skeeter_log(LOG_INFO, "Connection closed.");
    } else if (events & BEV_EVENT_ERROR) {
        skeeter_log(LOG_WARNING, "Got an error on the connection: %s",
                strerror(errno));
        skeeter_log(LOG_WARNING, "OpenSSL error is %lu", bufferevent_get_openssl_error(bev));
    } else if (events & BEV_EVENT_TIMEOUT) {
        skeeter_log(LOG_NOTICE, "Got a timeout on %s, closing connection.", (events & BEV_EVENT_READING) ? "reading" : "writing");
    } else if (events & BEV_EVENT_CONNECTED) {
        skeeter_log(LOG_NOTICE, "Looks like ssl handshake completed.");
        skeeter_log(LOG_DEBUG, "OpenSSL error %lu", bufferevent_get_openssl_error(bev));
        return;
    }
    skeeter_log(LOG_NOTICE, "Freeing connection data");
    bufferevent_free(bev);
    free(user_data);
}

static void
conn_readcb(struct bufferevent *bev, void *user_data)
{
    struct evbuffer *input = bufferevent_get_input(bev);
    struct imap_context *driver_ctx = user_data;
    int rc = IMAP_OK;

    skeeter_log(LOG_INFO, "Ready to read");
    while (rc == IMAP_OK) {
        struct evbuffer_ptr pos;
        struct imap_request *req;

        pos = evbuffer_search_eol(input, NULL, NULL, EVBUFFER_EOL_CRLF);
        if (pos.pos == -1)
            break;

        req = calloc(1, sizeof(struct imap_request));

        if (parse_request(req, input, pos.pos)) {
            skeeter_log(LOG_NOTICE, "invalid request");
            goto cleanup;
        }

        rc = imap_handle_request(driver_ctx, req);
        skeeter_log(LOG_INFO, "Request handled, result=%d", rc);

cleanup:
        request_free(req);
    }
}

/**
 * Parse the outline of an IMAP request.
 *
 * All IMAP requests are of the form:
 * REQ := TAG SP COMMAND ( SP ARG )* CRLF
 *
 * We cut the TAG SP COMMAND part and populate the req struct with them.
 */
static int
parse_request(struct imap_request *req, struct evbuffer *input, ssize_t eol)
{
    const char *p, *end;
    ssize_t len, to_drain = 0;

    p = evbuffer_pullup(input, eol);
    debug(LOG_DEBUG, "Parsing a new request: '%.*s'", p);

    end = memchr(p, ' ', eol);
    if (!end)
        return 1;

    len = end - p;
    ber_str2bv(p, 1, len, &req->tag);

    p = end + 1;
    eol -= len + 1;
    to_drain += len + 1;

    end = memchr(p, ' ', eol);
    if (!end) {
        len = eol
    } else {
        len = end - p;
    }

    ber_str2bv(p, 1, len, &req->command);
    to_drain += len;

    evbuffer_drain(input, to_drain);
    return 0;
#if 0
    struct evbuffer_ptr pos;
    const char *p;
    ssize_t len;

    debug(LOG_DEBUG, "Parsing a new request: '%.*s'", eol, evbuffer_pullup(input, eol));

    pos = evbuffer_search(input, " ", 1, NULL);
    if (pos.pos == -1)
        return 1;
    len = pos.pos;

    /* We also read the SP and replace it with a NUL afterwards */
    req->tag.bv_val = p = ber_memalloc(len + 1);
    req->tag.bv_len = len;
    eol -= evbuffer_remove(input, p, len + 1);
    p[len] = '\0';

    pos = evbuffer_search(input, " ", 1, NULL);
    if (pos.pos == -1)
        len = eol;
    else
        len = pos.pos;

    req->command.bv_val = p = ber_memalloc(len + 1);
    req->command.bv_len = len;
    buffer_remove(input, p, len);
    p[len] = '\0';

    return 0;
#endif
}

static void
request_free(struct imap_request *req)
{
    ber_memfree(req->tag.bv_val);
    ber_memfree(req->command.bv_val);
    free(req);
}

/**
 * Locates the EOL and drains the evbuffer up to and including the EOL string
 *
 * Returns:
 * -1 iff no EOL was found
 *  0 iff there was no data preceding the EOL
 *  n iff there were n bytes pending before the EOL
 */
static int
drain_newline(struct bufferevent *bev, enum evbuffer_eol_style eol_style)
{
    struct evbuffer_ptr pos;
    struct evbuffer *input;
    size_t eol_size;

    input = bufferevent_get_input(bev);

    pos = evbuffer_search_eol(input, NULL, &eol_size, eol_style);
    if (pos.pos != -1)
        evbuffer_drain(input, pos.pos + eol_size);
    return pos.pos;
}

static int
imap_driver_install(struct bufferevent *bev, struct imap_driver *driver)
{
    struct imap_context *ctx;
    struct timeval tval;

    ctx = (struct imap_context *)calloc(1, sizeof(struct imap_context));
    ctx->driver = driver;
    ctx->client_bev = bev;

    tval.tv_sec = 10;
    tval.tv_usec = 0;

    bufferevent_setcb(bev, conn_readcb, NULL, conn_eventcb, ctx);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    bufferevent_set_timeouts(bev, &tval, &tval);
    return IMAP_OK;
}

int
imap_handle_request(struct imap_context *ctx, struct imap_request *req)
{
    struct bufferevent *bev = ctx->client_bev;
    struct evbuffer *output = bufferevent_get_output(bev);
    struct imap_handler h = { req->command.bv_val, NULL };
    struct imap_handler *handler = avl_find(ctx->driver->commands, &h, imap_handler_cmp);

    if (handler == NULL) {
        skeeter_log(LOG_NOTICE, "No handler defined for command '%s'", req->command.bv_val);
        evbuffer_add_printf(output, "%s BAD Command %s unrecognized" CRLF, req->tag.bv_val, req->command.bv_val);
        return IMAP_OK;
    }

    if (handler->handler != NULL) {
        return handler->handler(ctx, req, handler->priv);
    }

    return IMAP_OK;
}

static int
imap_capability(struct imap_context *ctx, struct imap_request *req, void *priv)
{
    struct bufferevent *bev = ctx->client_bev;
    struct evbuffer *output = bufferevent_get_output(bev);

    if (drain_newline(bev, EVBUFFER_EOL_CRLF)) {
        evbuffer_add_printf(output, "%s BAD No arguments expected" CRLF, req->tag.bv_val);
        return IMAP_OK;
    }

    evbuffer_add_printf(output, "* CAPABILITY IMAP4rev1 STARTTLS AUTH=PLAIN LOGINDISABLED" CRLF);
    evbuffer_add_printf(output, "%s OK CAPABILITY completed" CRLF, req->tag.bv_val);

    return IMAP_OK;
}

static int
imap_starttls(struct imap_context *ctx, struct imap_request *req, void *priv)
{
    struct bufferevent *bev = ctx->client_bev;
    struct evbuffer *output = bufferevent_get_output(bev);
    SSL *ssl_client_ctx = SSL_new(ctx->driver->ssl_ctx);
    bufferevent_data_cb readcb, writecb;
    bufferevent_event_cb eventcb;
    void *orig_ctx;

    if (drain_newline(bev, EVBUFFER_EOL_CRLF)) {
        evbuffer_add_printf(output, "%s BAD No arguments expected" CRLF, req->tag.bv_val);
        return IMAP_OK;
    }

    if (ctx->state & IMAP_TLS) {
        evbuffer_add_printf(output, "%s BAD TLS layer already in place" CRLF, req->tag.bv_val);
        return IMAP_OK;
    }

    /* retrieve the callbacks to apply them again on the filtering bev */
    bufferevent_getcb(bev, &readcb, &writecb, &eventcb, &orig_ctx);

    evbuffer_add_printf(output, "%s OK Begin TLS negotiation now" CRLF, req->tag.bv_val);

    bev = bufferevent_openssl_filter_new(ctx->driver->base,
                                         bev, ssl_client_ctx,
                                         BUFFEREVENT_SSL_ACCEPTING,
                                         BEV_OPT_CLOSE_ON_FREE);

    if (!bev) {
        return IMAP_SHUTDOWN;
    }

    bufferevent_setcb(bev, readcb, writecb, eventcb, orig_ctx);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    ctx->client_bev = bev;
    ctx->state |= IMAP_TLS;

    return IMAP_OK;
}

static int
imap_login(struct imap_context *ctx, struct imap_request *req, void *priv)
{
    struct bufferevent *client_bev = ctx->client_bev;
    struct imap_arg *arg;

    arg = ctx->priv = calloc(2, sizeof(imap_arg));
    if (!arg)
        goto cleanup;

    chain = chain_new(imap_login_cleanup, ctx);
    if (!chain)
        goto cleanup;

    chain_add(chain, imap_sp, NULL, NULL);
    chain_add(chain, imap_astring, NULL, arg);

    arg++;
    arg->type = ARG_LAST;
    chain_add(chain, imap_sp, NULL, NULL);
    chain_add(chain, imap_astring, NULL, arg);

    chain_add(chain, imap_credential_check, NULL, ctx);
    chain_activate(chain, ctx->client_bev, EV_READ);
    return IMAP_DONE;

cleanup:
    free(ctx->priv);
    chain_destroy(chain);
    return IMAP_ERROR;
}

static int
imap_sp(struct chain *chain, struct bufferevent *bev, void *ctx)
{
    struct evbuffer *input = bufferevent_get_input(bev);
    char *p;
    rc = CHAIN_ERROR;

    if (!evbuffer_get_length(input))
        return CHAIN_AGAIN;

    p = evbuffer_pullup(input, 1);
    if (*p == ' ') {
        evbuffer_drain(input, 1);
        rc = CHAIN_DONE;
    }

    return rc;
}

static int
imap_astring(struct chain *chain, struct bufferevent *bev, void *ctx)
{
    struct evbuffer_ptr pos;
    imap_arg *dest = ctx;
    int eol_pos, end_pos;
    char *arg, *p;

    if ((dest->type & ARG_TYPES) == ARG_LITERAL) {
        int need = dest->arg_len - evbuffer_get_length(dest->buffer);

        /* Add as much data as possible to the buffer */
        need -= evbuffer_remove_buffer(input, dest->buffer, need);
        assert(need >= 0);

        return need ? CHAIN_AGAIN : CHAIN_DONE;
    }

    pos = evbuffer_search_eol(input, NULL, NULL, EVBUFFER_EOL_CRLF);
    eol_pos = pos.pos;

    if (arg->type & ARG_LAST) {
        end_pos = eol_pos;
    } else {
        pos = evbuffer_search(input, " ", 1, NULL);
        end_pos = pos.pos;
    }

    arg = evbuffer_pullup(input, 1);
    switch (*arg) {
        case '"':
            {
                char *quote = evbuffer_pullup(input, -1);

                dest->arg_type = ARG_QUOTED;
                do {
                    int escaped = 0;

                    quote = strchr(quote+1, '"');
                    if (!quote) {
                        if (eol_pos >= 0) {
                            /* invalid request */
                            return CHAIN_ERROR;
                        } else {
                            /* need more data */
                            return CHAIN_AGAIN;
                        }
                    }

                    /* find out whether it's escaped (= has an odd number of
                     * backslashes in front of it) */
                    p = quote;
                    while (*(--p) == '\\')
                        escaped = !escaped;
                } while (escaped);
            }
            break;
        case '{':
            dest->arg_type = ARG_LITERAL;

            /* find out how much data was requested */
            p = arg + 1;
            if (!(*p > '0' && *p <= '9')) {
                /* invalid */
            }
            while (*p >= '0' && *p <= '9')
                /* just skip over it */;

            /* parse the number and set dest->arg_len */
            /* receive the data */
            break;
        default:
            dest->arg_type = ARG_ATOM;
            if (end_pos == -1)
                return CHAIN_AGAIN;

            dest->arg_len = end_pos;
            evbuffer_remove_buffer(input, dest->buffer, end_pos);
            break;
    }
}

static int
imap_credential_check(struct chain *chain, struct bufferevent *bev, void *priv)
{
    struct imap_context *ctx = priv;
    struct imap_arg *args = ctx->priv;
    struct evbuffer *output;
    struct user_info user_info = {};
    char *attrs[2] = { "mailhost", NULL };
    char *p, *arg;
    ssize_t len, len_domain = 0;
    int rc = CHAIN_DONE;

    if (drain_newline(bev, EVBUFFER_EOL_CRLF)) {
        evbuffer_add_printf(output, "%s BAD Invalid number of arguments" CRLF, req->tag.bv_val);
        return rc;
    }

    output = bufferevent_get_output(client_bev);
    arg = req->arguments.bv_val;

    len = p - arg; // length of whole "username@domain"

    p = user_info.username.bv_val = evbuffer_pullup(args->arg_buf, -1);
    len = arg->arg_len;

    p = memchr(p, '@', len);
    if (p) {
        len = p - user_info.username.bv_val;
        len_domain = arg->arg_len - len - 1;
        p++;
    } else {
        // use a default domain
        p = ctx->driver->config->default_host;
    }
    user_info.username.bv_len = len;
    ber_str2bv(p, len_domain, 0, &(user_info.domain));

    user_info.attrs = attrs;
    if (get_user_info(ctx->driver->ldap, &user_info, search_cb, ctx)) {
        evbuffer_add_printf(output, "%s NO Internal server error", req->tag.bv_val);
        rc = IMAP_SHUTDOWN;
    }

    /* stop reading on the connection until we're connected to server */
    bufferevent_disable(client_bev, EV_READ);

    return rc;
}

static void
search_cb(LDAP *ld, LDAPMessage *msg, void *priv)
{
    struct imap_context *ctx = priv;
    struct bufferevent *server_bev;
    struct evbuffer *out;
    BerValue **servername = NULL;

    out = bufferevent_get_output(ctx->client_bev);
    if (msg) {
        servername = ldap_get_values_len(ld, msg, "mailhost");
    }

    // user not provisioned
    if (!servername || !*servername) {
        /*FIXME: need the full request to have somethng to send */
        evbuffer_add_printf(out, "some_tag " AUTH_FAILED_MSG CRLF);
        bufferevent_enable(ctx->client_bev, EV_READ);
        return;
    }

    server_bev = bufferevent_socket_new(ctx->driver->base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_enable(server_bev, EV_READ|EV_WRITE);
    bufferevent_socket_connect_hostname(server_bev, ctx->driver->dnsbase, AF_UNSPEC, servername[0]->bv_val, ctx->driver->config->default_port);
    bufferevent_setcb(server_bev, NULL, NULL, server_connect_cb, ctx);

    /*
    // copy over client data, CRLF in request has been skipped, so append that
    bufferevent_write(server_bev, req->line.bv_val, req->line.bv_len);
    bufferevent_write(server_bev, CRLF, 2);
    */

    ctx->server_bev = server_bev;
    ldap_value_free_len(servername);
}

static void
server_connect_cb(struct bufferevent *bev, short events, void *priv)
{
    struct imap_context *ctx = priv;
    // temporarily disable, until error_cb is ready
    //assert(bev == ctx->server_bev);

    if (events & BEV_EVENT_EOF) {
        skeeter_log(LOG_NOTICE, "Connection closed.");
    } else if (events & BEV_EVENT_ERROR) {
        skeeter_log(LOG_WARNING, "Got an error on the connection: %s",
                strerror(errno));
    } else if (events & BEV_EVENT_TIMEOUT) {
        skeeter_log(LOG_NOTICE, "Got a timeout on %s, closing connection.", (events & BEV_EVENT_READING) ? "reading" : "writing");
    } else if (events & BEV_EVENT_CONNECTED) {
        skeeter_log(LOG_NOTICE, "Looks like we are connected, proxying...");
        bufferevent_setcb(ctx->server_bev, proxy_cb, NULL, server_connect_cb, ctx);
        bufferevent_setcb(ctx->client_bev, proxy_cb, NULL, server_connect_cb, ctx);
        bufferevent_enable(ctx->client_bev, EV_READ);
        return;
    }
    skeeter_log(LOG_INFO, "Freeing connection data");
    bufferevent_free(ctx->server_bev);
    bufferevent_free(ctx->client_bev);
    free(ctx);
}

static void
proxy_cb(struct bufferevent *source, void *priv)
{
    struct imap_context *ctx = priv;
    struct evbuffer *input = bufferevent_get_input(source);
    /* pick the right direction, if reading from client_bev, dump to server_bev
     * and vice versa */
    struct bufferevent *target = (source == ctx->client_bev) ?
                                ctx->server_bev : ctx->client_bev;

    skeeter_log(LOG_INFO, "Proxying %zu bytes from %s.", evbuffer_get_length(input),
            (source == ctx->client_bev) ? "client" : "server");
    debug(LOG_DEBUG, "%.*s", evbuffer_get_length(input), evbuffer_pullup(input, -1));
    bufferevent_write_buffer(target, input);
}
