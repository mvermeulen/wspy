<?php
/**
 * Plugin Name: wspy auth bridge
 * Description: Works around a hosting-provider edge proxy dropping the
 * standard Authorization header before PHP ever sees it (confirmed live
 * on mvermeulen.org's IONOS shared hosting, 2026-07-31 -- see
 * web/wp_client.py in the wspy repo, https://github.com/mvermeulen/wspy,
 * for the full diagnosis). wspy-publish sends the identical Basic-Auth
 * value under a second, custom X-WSPY-Authorization header, which such
 * proxies have no reason to strip; this plugin copies it back into the
 * standard HTTP_AUTHORIZATION/PHP_AUTH_USER/PHP_AUTH_PW variables early
 * enough for WordPress's own Application Passwords REST auth to pick it
 * up exactly as if the original header had arrived normally. A no-op on
 * any request that doesn't carry the custom header (including on a host
 * where the standard Authorization header already works).
 *
 * Not part of the wspy codebase's own build/test -- install this file as
 * a WordPress plugin on the target site (Plugins -> upload, or drop into
 * wp-content/plugins/wspy-auth-bridge/ and activate).
 */

if (!empty($_SERVER['HTTP_X_WSPY_AUTHORIZATION']) && empty($_SERVER['HTTP_AUTHORIZATION'])) {
    $_SERVER['HTTP_AUTHORIZATION'] = $_SERVER['HTTP_X_WSPY_AUTHORIZATION'];
}

if (!empty($_SERVER['HTTP_AUTHORIZATION'])
    && stripos($_SERVER['HTTP_AUTHORIZATION'], 'Basic ') === 0
    && empty($_SERVER['PHP_AUTH_USER'])) {
    $decoded = base64_decode(substr($_SERVER['HTTP_AUTHORIZATION'], 6));
    if ($decoded !== false && strpos($decoded, ':') !== false) {
        list($_SERVER['PHP_AUTH_USER'], $_SERVER['PHP_AUTH_PW']) = explode(':', $decoded, 2);
    }
}
