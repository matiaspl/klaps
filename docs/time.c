#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t realtime_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return -1;

	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void common_headers(void)
{
	printf("Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n");
	printf("Pragma: no-cache\r\n");
	printf("Expires: Thu, 01 Jan 1970 00:00:00 GMT\r\n");
	printf("Access-Control-Allow-Origin: *\r\n");
	printf("Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n");
	printf("Access-Control-Allow-Headers: Content-Type\r\n");
}

int main(void)
{
	const int64_t receive_ns = realtime_ns();
	const char *method = getenv("REQUEST_METHOD");
	int64_t send_ns;
	int64_t midpoint_ns;
	time_t midpoint_seconds;
	struct tm utc;
	char iso_seconds[sizeof("YYYY-MM-DDTHH:MM:SS")];

	if (method == NULL)
		method = "GET";

	if (strcmp(method, "OPTIONS") == 0) {
		printf("Status: 204 No Content\r\n");
		common_headers();
		printf("\r\n");
		return 0;
	}

	if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
		printf("Status: 405 Method Not Allowed\r\n");
		common_headers();
		printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
		printf("Method not allowed\n");
		return 0;
	}

	send_ns = realtime_ns();
	if (receive_ns < 0 || send_ns < 0) {
		printf("Status: 500 Internal Server Error\r\n");
		common_headers();
		printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
		printf("Unable to read server clock\n");
		return 0;
	}

	midpoint_ns = receive_ns + (send_ns - receive_ns) / 2;
	midpoint_seconds = (time_t)(midpoint_ns / 1000000000LL);
	if (gmtime_r(&midpoint_seconds, &utc) == NULL ||
	    strftime(iso_seconds, sizeof(iso_seconds), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
		printf("Status: 500 Internal Server Error\r\n");
		common_headers();
		printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
		printf("Unable to format server clock\n");
		return 0;
	}

	printf("Content-Type: application/json; charset=utf-8\r\n");
	common_headers();
	printf("\r\n");

	if (strcmp(method, "HEAD") == 0)
		return 0;

	printf("{\"unix_ms\":%.3f,\"server_receive_ms\":%.3f,\"server_send_ms\":%.3f,"
	       "\"server_midpoint_ms\":%.3f,\"unix_seconds\":%.6f,\"iso\":\"%s.%03lldZ\"}\n",
	       midpoint_ns / 1000000.0, receive_ns / 1000000.0, send_ns / 1000000.0, midpoint_ns / 1000000.0,
	       midpoint_ns / 1000000000.0, iso_seconds, (long long)((midpoint_ns % 1000000000LL) / 1000000LL));

	return 0;
}
