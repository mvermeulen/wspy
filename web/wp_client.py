"""
web/wp_client.py -- minimal WordPress REST API client (stdlib urllib only,
matching this codebase's existing no-`requests`-dependency convention: see
wspy-analyze's Ollama client, joblib.py's fetch_openbenchmarking_xml()).

Scoped to what INVESTIGATION.md's 4.3 Tier 3 "static-site publishing
pipeline" item needs so far: authenticate via a WordPress Application
Password (HTTP Basic Auth over HTTPS, native since WP 5.6), look up or
create Pages, and walk doc/REPORT_HIERARCHY.md's suite/test/test-point/
machine levels as nested WP pages (parent/child), identifying an existing
page by (slug, parent) -- WordPress's own uniqueness rule for hierarchical
post types -- rather than a custom REST `meta` field, which would need a
WP-side plugin/mu-plugin to expose over the REST API and is more moving
parts than this needs.

Shared here (not inside wspy-publish itself) so web/server.py can reuse it
later for the web-UI-writes-reports item in the same tier -- the same
"shared module, thin CLI wrapper" convention web/joblib.py already
established for wspy-bundle/wspy-queue/wspy-analyze.

Host-specific auth quirk (found live on mvermeulen.org's IONOS shared
hosting, 2026-07-31): some hosts' edge proxies drop the `Authorization`
header entirely before PHP ever sees it (confirmed via a temporary debug
plugin dumping $_SERVER -- no HTTP_AUTHORIZATION/PHP_AUTH_USER/
REDIRECT_HTTP_AUTHORIZATION under any name), which defeats every
server-side recovery trick (`.htaccess` CGIPassAuth/RewriteRule, a
wp-config.php REDIRECT_HTTP_AUTHORIZATION fallback) since there is nothing
left in $_SERVER to recover. request() below works around this by also
sending the identical Basic-Auth value under a second, custom
`X-WSPY-Authorization` header, which such proxies have no reason to strip.
A small site-side plugin needs to copy that back into
HTTP_AUTHORIZATION/PHP_AUTH_USER/PHP_AUTH_PW early in the request so
WordPress's own Application Passwords code picks it up normally -- see
`scripts/wp-auth-bridge.php` (installed as a WordPress plugin on the
target site, not part of this codebase's own build/test). A harmless
no-op duplicate on hosts where the standard header already works.
"""
import base64
import json
import mimetypes
import os
import urllib.error
import urllib.parse
import urllib.request

REQUIRED_PAGE_CAPABILITIES = ("edit_pages", "edit_published_pages", "publish_pages", "upload_files")

CONFIG_PATH = os.path.expanduser("~/.config/wspy/publish.json")


def load_config():
    """~/.config/wspy/publish.json (mode 600, written by `wspy-publish
    configure`'s getpass prompt) -- {"wordpress": {"site_url", "username",
    "app_password"}, "report_root": ...}, or None if it doesn't exist yet.
    Shared here (not just in wspy-publish) so web/server.py's own "Publish
    to WordPress" button can read the same credentials without a second,
    web-facing way to enter an Application Password -- that stays a
    terminal-only `getpass` prompt (wspy-publish's own save_config()),
    deliberately not something this module writes."""
    if not os.path.isfile(CONFIG_PATH):
        return None
    with open(CONFIG_PATH) as f:
        return json.load(f)


class WPError(Exception):
    """A WordPress REST API call failed. Carries the HTTP status (None for
    a connection-level failure) and the site's own error code/message when
    it sent one, so callers can report something more useful than a raw
    traceback (e.g. "rest_cannot_edit_pages" -> "account is missing
    edit_pages")."""
    def __init__(self, message, status=None, code=None):
        super().__init__(message)
        self.status = status
        self.code = code


def auth_header(username, app_password):
    """Basic-auth header value for a WordPress Application Password.
    WordPress displays the password in space-separated groups of 4 for
    readability but ignores the spaces when checking it -- stripping them
    here is harmless either way and avoids surprises if a config file was
    hand-edited with the display spacing intact."""
    token = f"{username}:{app_password.replace(' ', '')}"
    return "Basic " + base64.b64encode(token.encode("utf-8")).decode("ascii")


def request(site_url, path, username, app_password, method="GET", params=None,
            json_body=None, raw_body=None, content_type=None, extra_headers=None, timeout=20):
    """One authenticated call to <site_url>/wp-json/<path>. Returns the
    parsed JSON response (or None for an empty body). Raises WPError on
    any non-2xx response or connection failure -- callers decide what
    "not found" vs. "denied" vs. "unreachable" means for their own
    operation rather than this function guessing.

    json_body (a dict) is the common case, JSON-encoded with a matching
    Content-Type. raw_body (bytes) + content_type is the escape hatch
    upload_media() uses to PUT/POST a file's raw bytes instead -- the two
    are mutually exclusive; callers pass one or neither, never both.

    Sends the Basic-Auth value under both the standard `Authorization`
    header and a second `X-WSPY-Authorization` header carrying the
    identical value. Some hosts (confirmed live on IONOS shared hosting,
    2026-07-31: `$_SERVER` has no HTTP_AUTHORIZATION/PHP_AUTH_USER/
    REDIRECT_HTTP_AUTHORIZATION under any name -- their edge proxy drops
    Authorization outright, not just renames it) strip the standard header
    before PHP ever sees it, defeating every server-side recovery trick
    (`.htaccess` CGIPassAuth/RewriteRule, a `wp-config.php`
    REDIRECT_HTTP_AUTHORIZATION fallback). The custom header survives on
    such hosts; a small site-side plugin (see wspy-publish's docstring)
    copies it back into HTTP_AUTHORIZATION/PHP_AUTH_USER/PHP_AUTH_PW so
    WordPress's own Application Passwords code picks it up normally. A
    no-op, harmless duplicate on hosts where Authorization already works."""
    url = site_url.rstrip("/") + "/wp-json/" + path.lstrip("/")
    if params:
        url += "?" + urllib.parse.urlencode(params)
    data = None
    auth = auth_header(username, app_password)
    headers = {"Authorization": auth, "X-WSPY-Authorization": auth,
               "User-Agent": "wspy-publish"}
    if json_body is not None:
        data = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif raw_body is not None:
        data = raw_body
        if content_type:
            headers["Content-Type"] = content_type
    if extra_headers:
        headers.update(extra_headers)
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
    except urllib.error.HTTPError as e:
        raw = e.read()
        message, code = str(e), None
        try:
            parsed = json.loads(raw)
            message = parsed.get("message", message)
            code = parsed.get("code")
        except (json.JSONDecodeError, AttributeError):
            pass
        raise WPError(message, status=e.code, code=code) from e
    except urllib.error.URLError as e:
        raise WPError(f"could not reach {site_url}: {e.reason}") from e
    if not body:
        return None
    return json.loads(body)


def whoami(site_url, username, app_password):
    """GET /wp/v2/users/me?context=edit -- the authenticated user's record
    including `capabilities`, so a caller can check for
    edit_pages/edit_published_pages/publish_pages/upload_files up front
    instead of discovering a missing one via a failed create call."""
    return request(site_url, "wp/v2/users/me", username, app_password,
                    params={"context": "edit"})


def missing_capabilities(user):
    """Subset of REQUIRED_PAGE_CAPABILITIES not present (or not truthy) in
    a whoami() result's `capabilities` map."""
    caps = (user or {}).get("capabilities", {})
    return [c for c in REQUIRED_PAGE_CAPABILITIES if not caps.get(c)]


def find_page(site_url, username, app_password, slug, parent):
    """Look up a Page by (slug, parent) -- returns the page dict, or None
    if no such page exists. parent=0 means top-level. status="any" so an
    existing draft is found too, not just published pages (avoids
    creating a duplicate of a page that's merely unpublished)."""
    results = request(site_url, "wp/v2/pages", username, app_password,
                       params={"slug": slug, "parent": parent, "status": "any"})
    return results[0] if results else None


def create_page(site_url, username, app_password, slug, title, parent,
                 content="", status="draft"):
    """POST a new Page. Requires edit_pages (status="draft") /
    publish_pages (status="publish") on the authenticated account."""
    return request(site_url, "wp/v2/pages", username, app_password, method="POST",
                    json_body={"slug": slug, "title": title, "parent": parent,
                               "content": content, "status": status})


def get_page(site_url, username, app_password, page_id):
    """GET a Page by id -- verifies it exists / fetches its current state
    (status/link/content) without changing anything."""
    return request(site_url, f"wp/v2/pages/{page_id}", username, app_password)


def update_page(site_url, username, app_password, page_id, **fields):
    """POST changed fields (content/title/status/...) onto an existing
    Page by id."""
    return request(site_url, f"wp/v2/pages/{page_id}", username, app_password,
                    method="POST", json_body=fields)


def publish_page(site_url, username, app_password, page_id):
    """Flip an existing Page from draft to published -- the second POST in
    the draft-first publish flow (INVESTIGATION.md 4.3 Tier 3 item 2,
    sub-step 5): create as draft, verify the response (id/link present),
    optionally populate content/meta, only then call this. Kept as its own
    call (not folded into create_page's status= argument) so a pipeline
    that dies between the two POSTs leaves a safely-inspectable draft
    rather than a half-written live page. Requires publish_pages on the
    authenticated account."""
    return update_page(site_url, username, app_password, page_id, status="publish")


def delete_page(site_url, username, app_password, page_id, force=False):
    """DELETE a Page by id. Moves it to the trash by default (WordPress's
    own reversible default for pages -- recoverable from wp-admin's Trash
    view); pass force=True to bypass the trash and delete permanently."""
    params = {"force": "true"} if force else None
    return request(site_url, f"wp/v2/pages/{page_id}", username, app_password,
                    method="DELETE", params=params)


def publish_page_content(site_url, username, app_password, slug, parent, title, content,
                          do_publish=False):
    """Find-or-create a Page by (slug, parent), set its content/title, and
    optionally flip it to published -- the shared orchestration behind both
    wspy-publish publish-page's --slug path and web/server.py's "Publish to
    WordPress" button, so the CLI and the web UI can never drift into
    different find-or-create/publish behavior for the same operation.

    Unlike publish-page's --page-id path (which only touches fields
    actually given, so a bare `--publish` can flip status without
    disturbing existing content), this always overwrites content/title --
    right for a caller that just rendered fresh content and wants it to
    win, wrong for an interactive "just change one field" edit. Returns
    (page_dict, created_bool). Raises WPError on any failure -- the caller
    decides how to present that (CLI stderr vs. an HTML error panel)."""
    page = find_page(site_url, username, app_password, slug, parent)
    created = page is None
    if created:
        page = create_page(site_url, username, app_password, slug, title, parent,
                            content=content, status="draft")
    else:
        page = update_page(site_url, username, app_password, page["id"],
                            content=content, title=title)
    if do_publish:
        page = publish_page(site_url, username, app_password, page["id"])
    return page, created


def find_or_create_page_path(site_url, username, app_password, levels, status="draft"):
    """Walk doc/REPORT_HIERARCHY.md's hierarchy top-down as nested WP
    pages. `levels` is an ordered list of (slug, title) pairs, one per
    level (e.g. [("cpu2017", "cpu2017"), ("500.perlbench_r", "500.perlbench_r"), ...]).
    Each level is looked up under the previous level's page id as
    `parent` (0 for the first level) and created only if missing. Returns
    a list of (page_dict, created_bool) pairs, same order as `levels`, so
    a caller can tell which levels already existed (e.g. hand-created
    suite stub pages) from which this call just created."""
    parent = 0
    out = []
    for slug, title in levels:
        page = find_page(site_url, username, app_password, slug, parent)
        created = False
        if page is None:
            page = create_page(site_url, username, app_password, slug, title, parent,
                                status=status)
            created = True
        out.append((page, created))
        parent = page["id"]
    return out


def publish_page_at_path(site_url, username, app_password, levels, content, do_publish=False):
    """Walk levels[:-1] top-down, creating each as an empty draft stub only if missing (never
    overwriting an existing page, e.g. a hand-created suite stub) -- the same find-or-create logic
    find_or_create_page_path() already uses -- then publish_page_content() the final (leaf) level
    with real content. Neither existing primitive covers this alone: find_or_create_page_path() has
    no content parameter, and publish_page_content() only operates at one flat (slug, parent) level
    with no walk. Closes that gap (INVESTIGATION.md Tier 3 item 6) for callers that build a page at
    an exact hierarchy path, e.g. levels=[("cpu2026", "cpu2026"), ("706.stockfish_r",
    "706.stockfish_r")]. Returns (page_dict, created_bool) for the leaf page only, same shape as
    publish_page_content()."""
    *parent_levels, (leaf_slug, leaf_title) = levels
    parent_id = 0
    for slug, title in parent_levels:
        page = find_page(site_url, username, app_password, slug, parent_id)
        if page is None:
            page = create_page(site_url, username, app_password, slug, title, parent_id)
        parent_id = page["id"]
    return publish_page_content(site_url, username, app_password, leaf_slug, parent_id, leaf_title,
                                 content, do_publish=do_publish)


def upload_media(site_url, username, app_password, file_path, mime_type=None, timeout=60):
    """POST a file's raw bytes to /wp/v2/media -- WordPress's documented
    "raw binary" upload method (Content-Type set to the file's MIME type,
    plus a Content-Disposition: attachment header carrying the filename)
    rather than multipart/form-data, since a single-file raw body needs no
    extra library beyond stdlib mimetypes (INVESTIGATION.md 4.3 Tier 3
    item 2, sub-step 6 -- needed before a report page can embed the
    topdown/AMD-metrics charts or process-tree diagrams the hand-curated
    /perf/workloads pages already have). Returns the created attachment
    (id, source_url, ...) for referencing via a page's featured_media or
    inline in Gutenberg block markup (`<!-- wp:image {"id":<id>} -->`).
    Requires upload_files on the authenticated account.

    Uploaded media publishes immediately -- WordPress has no draft state
    for attachments -- so unlike publish_page()'s two-step flow, this call
    itself is the only step; keep pages referencing it in draft until the
    page itself is ready to go live."""
    if mime_type is None:
        mime_type, _ = mimetypes.guess_type(file_path)
        if mime_type is None:
            raise WPError('could not guess a MIME type for "%s"; pass mime_type= explicitly' % file_path)
    with open(file_path, "rb") as f:
        data = f.read()
    filename = os.path.basename(file_path)
    return request(site_url, "wp/v2/media", username, app_password, method="POST",
                    raw_body=data, content_type=mime_type,
                    extra_headers={"Content-Disposition": 'attachment; filename="%s"' % filename},
                    timeout=timeout)


def update_media(site_url, username, app_password, media_id, **fields):
    """POST changed fields (title/alt_text/caption/...) onto an existing
    Media attachment by id -- the raw upload_media() call carries no room
    for these (its body is the file's raw bytes, not JSON), so setting
    them is always this separate follow-up call."""
    return request(site_url, f"wp/v2/media/{media_id}", username, app_password,
                    method="POST", json_body=fields)
