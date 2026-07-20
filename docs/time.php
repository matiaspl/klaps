<?php
$receive_ms = microtime(true) * 1000.0;

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('Pragma: no-cache');
header('Expires: Thu, 01 Jan 1970 00:00:00 GMT');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, HEAD, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
	http_response_code(204);
	exit;
}

$send_ms = microtime(true) * 1000.0;
$midpoint_ms = ($receive_ms + $send_ms) / 2.0;
$midpoint_seconds = (int)floor($midpoint_ms / 1000.0);
$midpoint_fraction_ms = (int)floor(fmod($midpoint_ms, 1000.0));

echo json_encode([
	'unix_ms' => $midpoint_ms,
	'server_receive_ms' => $receive_ms,
	'server_send_ms' => $send_ms,
	'server_midpoint_ms' => $midpoint_ms,
	'unix_seconds' => $midpoint_ms / 1000.0,
	'iso' => gmdate('Y-m-d\TH:i:s', $midpoint_seconds) . sprintf('.%03dZ', $midpoint_fraction_ms),
]);
echo "\n";
