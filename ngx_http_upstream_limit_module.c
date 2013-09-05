#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
  ngx_uint_t                            max_tries;

  ngx_http_upstream_init_pt             original_init_upstream;
  ngx_http_upstream_init_peer_pt        original_init_peer;
} ngx_http_upstream_limit_srv_conf_t;

typedef struct {
  ngx_http_upstream_limit_srv_conf_t   *conf;

  ngx_uint_t                            tries;

  void                                 *data;

  ngx_event_get_peer_pt                 original_get_peer;
  ngx_event_free_peer_pt                original_free_peer;

#if (NGX_HTTP_SSL)
  ngx_event_set_peer_session_pt         original_set_session;
  ngx_event_save_peer_session_pt        original_save_session;
#endif
} ngx_http_upstream_limit_peer_data_t;

static ngx_int_t ngx_http_upstream_init_limit_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_limit_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_limit_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);

#if (NGX_HTTP_SSL)
static ngx_int_t ngx_http_upstream_limit_set_session(
    ngx_peer_connection_t *pc, void *data);
static void ngx_http_upstream_limit_save_session(ngx_peer_connection_t *pc,
    void *data);
#endif

static void *ngx_http_upstream_limit_create_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_max_retries(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_command_t  ngx_http_upstream_limit_commands[] = {

  { ngx_string("max_retries"),
    NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
    ngx_http_upstream_max_retries,
    NGX_HTTP_SRV_CONF_OFFSET,
    0,
    NULL },

  ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_limit_module_ctx = {
  NULL,                                   /* preconfiguration */
  NULL,                                   /* postconfiguration */

  NULL,                                   /* create main configuration */
  NULL,                                   /* init main configuration */

  ngx_http_upstream_limit_create_conf,    /* create server configuration */
  NULL,                                   /* merge server configuration */

  NULL,                                   /* create location configuration */
  NULL                                    /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_limit_module = {
  NGX_MODULE_V1,
  &ngx_http_upstream_limit_module_ctx,    /* module context */
  ngx_http_upstream_limit_commands,       /* module directives */
  NGX_HTTP_MODULE,                        /* module type */
  NULL,                                   /* init master */
  NULL,                                   /* init module */
  NULL,                                   /* init process */
  NULL,                                   /* init thread */
  NULL,                                   /* exit thread */
  NULL,                                   /* exit process */
  NULL,                                   /* exit master */
  NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_upstream_init_limit(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
  ngx_http_upstream_limit_srv_conf_t       *lcf;

  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
                 "init upstream limit");

  lcf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_limit_module);

  if (lcf->original_init_upstream(cf, us) != NGX_OK) {
    return NGX_ERROR;
  }

  lcf->original_init_peer = us->peer.init;

  us->peer.init = ngx_http_upstream_init_limit_peer;

  return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_init_limit_peer(ngx_http_request_t *r,
                                  ngx_http_upstream_srv_conf_t *us)
{
  ngx_http_upstream_limit_srv_conf_t   *lcf;
  ngx_http_upstream_limit_peer_data_t  *lp;

  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "init upstream limit peer");

  lcf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_limit_module);

  lp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_limit_peer_data_t));
  if (lp == NULL) {
    return NGX_ERROR;
  }

  if (lcf->original_init_peer(r, us) != NGX_OK) {
    return NGX_ERROR;
  }

  lp->tries = 0;
  lp->conf = lcf;
  lp->data = r->upstream->peer.data;
  lp->original_get_peer = r->upstream->peer.get;
  lp->original_free_peer = r->upstream->peer.free;

  r->upstream->peer.data = lp;
  r->upstream->peer.get = ngx_http_upstream_get_limit_peer;
  r->upstream->peer.free = ngx_http_upstream_free_limit_peer;

#if (NGX_HTTP_SSL)
  lp->original_set_session = r->upstream->peer.set_session;
  lp->original_save_session = r->upstream->peer.save_session;
  r->upstream->peer.set_session = ngx_http_upstream_limit_set_session;
  r->upstream->peer.save_session = ngx_http_upstream_limit_save_session;
#endif

  return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_get_limit_peer(ngx_peer_connection_t *pc, void *data)
{
  ngx_http_upstream_limit_peer_data_t  *lp = data;

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                 "get upstream limit peer, try: %ui", lp->tries);

  lp->tries ++;

  return lp->original_get_peer(pc, lp->data);
}

static void
ngx_http_upstream_free_limit_peer(ngx_peer_connection_t *pc, void *data,
                                  ngx_uint_t state)
{
  ngx_http_upstream_limit_srv_conf_t   *lcf;
  ngx_http_upstream_limit_peer_data_t  *lp = data;

  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                 "free upstream limit peer");

  lp->original_free_peer(pc, lp->data, state);

  lcf = lp->conf;

  if ((state & NGX_PEER_FAILED) && pc->tries) {
    if (lcf->max_tries && lp->tries >= lcf->max_tries) {
      pc->tries = 0;

      ngx_log_error(NGX_LOG_ERR, pc->log, 0,
                    "retry disabled after %ui fail(s)", lp->tries);
    }
  }
}

#if (NGX_HTTP_SSL)
static ngx_int_t
ngx_http_upstream_limit_set_session(ngx_peer_connection_t *pc, void *data)
{
  ngx_http_upstream_limit_peer_data_t  *lp = data;

  return lp->original_set_session(pc, lp->data);
}

static void
ngx_http_upstream_limit_save_session(ngx_peer_connection_t *pc, void *data)
{
  ngx_http_upstream_limit_peer_data_t  *lp = data;

  lp->original_save_session(pc, lp->data);
  return;
}
#endif

static void *
ngx_http_upstream_limit_create_conf(ngx_conf_t *cf)
{
  ngx_http_upstream_limit_srv_conf_t  *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_limit_srv_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  /*
   * set by ngx_pcalloc():
   *
   *     conf->max_tries = 0;
   *     conf->original_init_upstream = NULL;
   *     conf->original_init_peer = NULL;
   */

  return conf;
}

static char *
ngx_http_upstream_max_retries(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t            *uscf;
  ngx_http_upstream_limit_srv_conf_t      *lcf = conf;

  ngx_int_t     n;
  ngx_str_t    *value;

  uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  if (lcf->original_init_upstream) {
    return "is duplicate";
  }

  lcf->original_init_upstream = uscf->peer.init_upstream
                                ? uscf->peer.init_upstream
                                : ngx_http_upstream_init_round_robin;

  uscf->peer.init_upstream = ngx_http_upstream_init_limit;

  /* read options */

  value = cf->args->elts;

  n = ngx_atoi(value[1].data, value[1].len);

  if (n == NGX_ERROR) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid value \"%V\" in \"%V\" directive",
                       &value[1], &cmd->name);
    return NGX_CONF_ERROR;
  }

  lcf->max_tries = n + 1;

  return NGX_CONF_OK;
}
