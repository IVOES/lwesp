/**
 * \file            esp_rest_client.c
 * \brief           HTTP REST client based on NETCONN API
 */

/*
 * Copyright (c) 2018 Tilen Majerle
 *  
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software, 
 * and to permit persons to whom the Software is furnished to do so, 
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of ESP-AT.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 */
#include "esp/esp_rest_client.h"
#include "esp/esp_netconn.h"
#include "esp/esp_mem.h"

#if ESP_CFG_REST_CLIENT || __DOXYGEN__
#if !ESP_CFG_NETCONN
#error To use REST_CLIENT API, ESP_CFG_NETCONN feature must be enabled!
#endif /* !ESP_CFG_NETCONN */

/**
 * \brief           Execute REST call and pass everything in single shot
 * \param[in]       m: HTTP method used in request header
 * \param[in]       uri: URI to open, including "http[s]://". Example: "http://example.com:80/test/data?param1=param2..."
 * \param[in]       tx_data: Optional TX data to send. Usually not used on `GET` method
 * \param[in]       tx_len: Optional length of TX data in units of bytes
 * \param[out]      http_code: HTTP code received from server
 * \param[out]      p: Pointer to pbuf for output data
 * \param[out]      p_off: Offset position in output pbuf where HTTP data part exists, skipping headers
 * \param[in]       cb: Pointer to callbacks used for request
 * \return          \ref espr_t on success, member of \ref espr_t otherwise
 */
espr_t
esp_rest_execute(esp_http_method_t m, const char* uri, const void* tx_data, size_t tx_len, esp_rest_resp_t* r, void* arg) {
    esp_netconn_p nc;
    uint8_t is_ssl;
    const char *uri_domain, *uri_domain_end, *uri_path;
    char* domain;
    size_t uri_domain_len, uri_path_len;
    esp_port_t port;
    espr_t res;
    esp_pbuf_p pbuf;

    ESP_ASSERT("uri != NULL", uri != NULL);     /* Check input parameters */
    ESP_ASSERT("r != NULL", r != NULL);         /* Check input parameters */
    if (tx_len > 0) {                           /* In case of any length passed */
        ESP_ASSERT("tx_data != NULL", tx_data != NULL); /* Check input parameters */
    }

    r->p = NULL;                                /* Reset pbuf pointer */

    /* Check for SSL/TCP first */
    uri_domain = NULL;
    if (!strncmp(uri, "https://", 8)) {
        is_ssl = 1;
        uri_domain = &uri[8];
        uri += 8;
    } else if (!strncmp(uri, "http://", 7)) {
        is_ssl = 0;
        uri_domain = &uri[7];
        uri += 7;
    } else {
        return espERR;
    }

    /* Check for domain and domain length */
    if (uri_domain != NULL) {
        uri_domain_end = strchr(uri_domain, ':');   /* Scan for port if exists */
        if (uri_domain_end == NULL) {           /* There is no port specified, use default one */
            uri_domain_end = strchr(uri_domain, '/');   /* Check for request URI part for domain */
        }
        if (uri_domain_end != NULL) {
            uri_domain_len = uri_domain_end - uri_domain;
        } else {
            uri_domain_len = strlen(uri_domain);
        }
        uri += uri_domain_len;                  /* Advance uri for domain length */
    } else {
        return espERR;
    }

    /* Check for port */
    if (*uri == ':') {                          /* Check if port is specified */
        uri++;
        port = 0;
        while (uri != NULL && *uri >= '0' && *uri <= '9') {
            port = 10 * port + (*uri - '0');
            uri++;
        }
    } else {
        port = is_ssl ? 443 : 80;
    }

    /* Check for request uri, including parameters */
    if (*uri == '/') {
        uri_path = uri;
        uri_path_len = strlen(uri);
    } else {
        uri_path = "/";
        uri_path_len = 1;
    }

    /* Allocate memory for domain */
    domain = esp_mem_alloc(sizeof(*domain) * (uri_domain_len + 1));
    if (domain != NULL) {
        ESP_MEMCPY(domain, uri_domain, uri_domain_len);
        domain[uri_domain_len] = 0;
    } else {
        return espERRMEM;
    }

    /*
     * Start netconn and connect to server
     */
    nc = esp_netconn_new(is_ssl ? ESP_NETCONN_TYPE_SSL : ESP_NETCONN_TYPE_TCP);
    if (nc != NULL) {
        res = esp_netconn_connect(nc, domain, port);
        if (res == espOK) {
            uint8_t check_http_code = 1, check_headers_end = 1;
            
            /* Request method + uri + HTTP version */
            switch (m) {
                case ESP_HTTP_METHOD_POST:      esp_netconn_write(nc, "POST", 4);       break;
                case ESP_HTTP_METHOD_PUT:       esp_netconn_write(nc, "PUT", 3);        break;
                case ESP_HTTP_METHOD_CONNECT:   esp_netconn_write(nc, "CONNECT", 7);    break;
                case ESP_HTTP_METHOD_DELETE:    esp_netconn_write(nc, "DELETE", 6);     break;
                case ESP_HTTP_METHOD_HEAD:      esp_netconn_write(nc, "HEAD", 4);       break;
                case ESP_HTTP_METHOD_OPTIONS:   esp_netconn_write(nc, "OPTIONS", 7);    break;
                case ESP_HTTP_METHOD_PATCH:     esp_netconn_write(nc, "PATCH", 5);      break;
                case ESP_HTTP_METHOD_TRACE:     esp_netconn_write(nc, "TRACE", 5);      break;
                case ESP_HTTP_METHOD_GET:       
                default:                        esp_netconn_write(nc, "GET", 3);        break;
            }
            esp_netconn_write(nc, " ", 1);
            esp_netconn_write(nc, uri_path, uri_path_len);
            esp_netconn_write(nc, " HTTP/1.1\r\n", 11);

            /* Host */
            esp_netconn_write(nc, "Host: ", 6);
            esp_netconn_write(nc, uri_domain, uri_domain_len);
            esp_netconn_write(nc, "\r\n", 2);

            esp_netconn_write(nc, "Connection: close\r\n", 19); /* Connection close */
            
            if (tx_len && tx_data != NULL) {    /* Content length */
                char tx_len_str[11];
                sprintf(tx_len_str, "%d", (int)tx_len);
                esp_netconn_write(nc, "Content-Length: ", 16);
                esp_netconn_write(nc, tx_len_str, strlen(tx_len_str));
                esp_netconn_write(nc, "\r\n", 2);
            }
            
            esp_netconn_write(nc, "\r\n", 2);   /* End of headers */
    
            /* Send user data if exists */
            if (tx_len && tx_data != NULL) {
                esp_netconn_write(nc, tx_data, tx_len);
            }

            /* Flush and force send everything */
            esp_netconn_flush(nc);              /* Flush and force send */

            /* Handle received data */
            while (1) {
                res = esp_netconn_receive(nc, &pbuf);   /* Receive new packet of data */

                if (res == espOK) {             /* We have new data */
                    if (r->p == NULL) {         /* Check if we already have first buffer */
                        r->p = pbuf;            /* Set as first buffer */
                    } else {
                        esp_pbuf_cat(r->p, pbuf);   /* Concat buffers together */
                    }
                } else {
                    if (res == espCLOSED) {     /* Connection closed at this point */
                        res = espOK;
                    }
                    break;
                }
                
                /*
                 * Check if we can detect HTTP response code
                 *
                 * Response is: "HTTP/1.1 code", minimum `12` characters
                 */
                if (check_http_code && r->p != NULL &&
                    esp_pbuf_length(r->p, 1) >= 12 && !esp_pbuf_memcmp(r->p, "HTTP/", 5, 0)) {
                    size_t pos = 9;
                    uint8_t el;

                    r->http_code = 0;
                    while (1) {
                        /* Get entry for HTTP code */
                        if (!esp_pbuf_get_at(r->p, pos++, &el) || (el < '0' || el > '9')) {
                            break;
                        }
                        r->http_code = 10 * (r->http_code) + (el - '0');
                    }
                    check_http_code = 0;        /* No need to check for HTTP code anymore */
                }
                    
                /*
                 * Calculate offset in pbuf where actual data start
                 */
                if (check_headers_end && r->p != NULL) {
                    r->p_offset = esp_pbuf_memfind(r->p, "\r\n\r\n", 4, 0);
                    if (r->p_offset != ESP_SIZET_MAX) {
                        check_headers_end = 0;  /* No need to check for headers anymore */
                        r->p_offset += 4;
                    } else {
                        r->p_offset = 0;
                    }
                }
            }
        }
        esp_netconn_delete(nc);                 /* Delete netconn connection */
    } else {
        res = espERRMEM;
    }
    if (domain != NULL) {                       /* Clear domain memory */
        esp_mem_free(domain);
        domain = NULL;
    }
    return res;
}

#endif /* ESP_CFG_REST_CLIENT || __DOXYGEN__ */