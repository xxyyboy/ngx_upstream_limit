Name
====

ngx_upstream_limit - Limit for nginx upstream retry.

Synopsis
========

    http {
        upstream mybk {
            server myserver1;
            server myserver2;

            max_retries 0;
        }

        server {
            location / {
                proxy_pass http://mybk;
            }
        }
    }

Directives
==========

max_retries
-----------
**syntax:** *max_retries number;*
**default:** *-*
**context:** *upstream*

Limit the retries to backend. Number 0 means retry disabled.
