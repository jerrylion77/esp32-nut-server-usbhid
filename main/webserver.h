#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"

// Start the webserver
esp_err_t webserver_start(void);

// Webserver resilience logic: error burst handler
void handle_accept_error(void);

#endif // WEBSERVER_H 