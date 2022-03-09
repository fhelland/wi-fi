#pragma once
#ifndef FILE_SERVER_H_INCLUDED
#define FILE_SERVER_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif



/* Extract base path from input path (remove item behind last traling forward slash including the forward slash */
/* input:   const char *path        */
/*          size_t destsize         */
/* output: char *dest               */
void get_base_path(char *dest, const char *path, size_t destsize);

/* Notify connection task that connection is done (or failed) and we can send status back to http web page */
void xTask_connection_give();//( UBaseType_t uxIndexToNotify );

/* Start the http server main task */
esp_err_t start_file_server(const char *base_path);



#ifdef __cplusplus
}
#endif


#endif  /* FILE_SERVER_H_INCLUDED   */