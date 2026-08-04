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
import hashlib
import json
import mimetypes
import os
import stat
import urllib.error
import urllib.parse
import urllib.request

REQUIRED_PAGE_CAPABILITIES = ("edit_pages", "edit_published_pages", "publish_pages", "upload_files")

CONFIG_PATH = os.path.expanduser("~/.config/wspy/publish.json")
PUBLISH_STATE_PATH = os.path.expanduser("~/.config/wspy/publish_state.json")


def load_config():
    """~/.config/wspy/publish.json (mode 600, written by `wspy-publish
    configure`'s getpass prompt) -- {"wordpress": {"site_url", "username",
    "app_password"}, "report_root": ..., "machine_short_name": ...
    (optional -- this machine's doc/REPORT_HIERARCHY.md catalog name once
    registered via scripts/publish_machine_page.py, so later tools default
    to it instead of asking again every time)}, or None if it doesn't
    exist yet. Shared here (not just in wspy-publish) so web/server.py's
    own "Publish to WordPress" button can read the same credentials
    without a second, web-facing way to enter an Application Password --
    entering the Application Password itself stays a terminal-only
    `getpass` prompt (wspy-publish configure), never something a web
    form or this module writes on its own."""
    if not os.path.isfile(CONFIG_PATH):
        return None
    with open(CONFIG_PATH) as f:
        return json.load(f)


def save_config(cfg):
    """Writes cfg to CONFIG_PATH as mode 600 (owner read/write only) -- the Application Password
    lives in this file, so it's created with restrictive permissions from the first write rather
    than briefly existing world-readable between os.open() and a later chmod(). Shared here (moved
    from wspy-publish, its original sole caller) since scripts/publish_machine_page.py now also
    needs to persist a field (machine_short_name) without duplicating this same open/dump/chmod
    dance a second time."""
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    fd = os.open(CONFIG_PATH, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.chmod(CONFIG_PATH, stat.S_IRUSR | stat.S_IWUSR)


def load_publish_state():
    """{"<page_id>": "<sha256 hex digest of the content wspy itself last confirmed live on that
    page>"} -- PUBLISH_STATE_PATH, sibling to CONFIG_PATH but kept separate since it holds no
    secrets (no mode 600 needed) and grows one entry per published page rather than staying a fixed
    handful of config fields. {} if missing/unparseable, same "always safe to start fresh" contract
    load_config() doesn't need but this does: publish_page_content()'s drift check treats an unknown
    page id as "no fingerprint on record yet" and skips protection for that one page rather than
    failing, so a reset/missing state file just means every page gets one untracked publish before
    tracking resumes -- never a hard failure."""
    try:
        with open(PUBLISH_STATE_PATH) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_publish_state(state):
    """Atomic write (tmp + os.replace) so a crash mid-write can never leave a truncated/corrupt state
    file behind for the next load_publish_state() to choke on."""
    os.makedirs(os.path.dirname(PUBLISH_STATE_PATH), exist_ok=True)
    tmp = PUBLISH_STATE_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
        f.write("\n")
    os.replace(tmp, PUBLISH_STATE_PATH)


def _content_hash(content):
    return hashlib.sha256((content or "").encode("utf-8")).hexdigest()


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


class WPContentDriftError(WPError):
    """Raised by publish_page_content() when an existing page's live content no longer matches the
    fingerprint wspy itself recorded from its own last write to that page id -- a human likely
    hand-edited it in wp-admin since. Carries page_id/link so a caller can point back at the live
    page in its own error message. Pass force=True to publish_page_content()/publish_page_at_path()
    to overwrite anyway -- the same "never silently overwrite a human's own edits, but let a human
    explicitly override" idiom this codebase already uses for run-role human_set overrides
    (wspy-testpoint) and curation.json's preserved per-block commentary."""
    def __init__(self, page_id, link):
        super().__init__(
            "page content has changed since wspy last published it -- likely hand-edited since; "
            "pass force=True to overwrite anyway", code="wspy_content_drift")
        self.page_id = page_id
        self.link = link


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


def list_child_pages(site_url, username, app_password, parent):
    """All Pages directly under `parent` (0 for top-level) -- GET /wp/v2/pages?parent=<id>,
    paginated at per_page=100 (WordPress REST's own max) since a test-point page could plausibly
    accumulate more machine-level children than the default per_page=10. status="any" so a
    still-draft machine page is reported too, matching find_page()'s own convention. Symmetric to
    find_page()'s single (slug, parent) lookup -- this is the "what's actually posted under here"
    read the reference-matrix database's per-cell publish-status column needs (INVESTIGATION.md 4.3
    Tier 3 item 5), rather than a second, separately-maintained local record of what's been
    published."""
    pages = []
    page_num = 1
    while True:
        results = request(site_url, "wp/v2/pages", username, app_password,
                           params={"parent": parent, "status": "any", "per_page": 100,
                                   "page": page_num})
        if not results:
            break
        pages.extend(results)
        if len(results) < 100:
            break
        page_num += 1
    return pages


def create_page(site_url, username, app_password, slug, title, parent,
                 content="", status="draft"):
    """POST a new Page. Requires edit_pages (status="draft") /
    publish_pages (status="publish") on the authenticated account."""
    return request(site_url, "wp/v2/pages", username, app_password, method="POST",
                    json_body={"slug": slug, "title": title, "parent": parent,
                               "content": content, "status": status})


def get_page(site_url, username, app_password, page_id, context=None):
    """GET a Page by id -- verifies it exists / fetches its current state (status/link/content)
    without changing anything. context="edit" additionally requests content.raw (the actual stored
    block markup) alongside content.rendered (sanitized HTML) -- needed by publish_page_content()'s
    drift check, which must compare against exactly what WordPress has stored, not its rendered
    view."""
    params = {"context": context} if context else None
    return request(site_url, f"wp/v2/pages/{page_id}", username, app_password, params=params)


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


def _live_raw_content(site_url, username, app_password, page_id):
    """Re-fetches a page's own content.raw via an explicit context="edit" GET -- the exact bytes
    WordPress currently has stored. Always used as the fingerprint basis (both checking for drift and
    recording a fresh baseline after a write) rather than trusting a caller's own content string
    verbatim, since WordPress can reformat/normalize block markup on save -- anchoring every
    comparison to what the server actually reports avoids a false "drift" positive on the very next
    publish of unchanged content."""
    page = get_page(site_url, username, app_password, page_id, context="edit")
    return (page.get("content") or {}).get("raw", "")


def _check_no_drift(site_url, username, app_password, page_id, link):
    """Raises WPContentDriftError if page_id's live content no longer matches the fingerprint
    recorded from wspy's own last write. A page with no fingerprint on record (predates this feature,
    or was never published by wspy before) is trusted rather than blocked -- protection only applies
    going forward from the first publish that records a baseline."""
    known_hash = load_publish_state().get(str(page_id))
    if known_hash is None:
        return
    if _content_hash(_live_raw_content(site_url, username, app_password, page_id)) != known_hash:
        raise WPContentDriftError(page_id, link)


def _record_publish_fingerprint(site_url, username, app_password, page_id):
    state = load_publish_state()
    state[str(page_id)] = _content_hash(_live_raw_content(site_url, username, app_password, page_id))
    save_publish_state(state)


def publish_page_content(site_url, username, app_password, slug, parent, title, content,
                          do_publish=False, force=False):
    """Find-or-create a Page by (slug, parent), set its content/title, and
    optionally flip it to published -- the shared orchestration behind both
    wspy-publish publish-page's --slug path and web/server.py's "Publish to
    WordPress" button, so the CLI and the web UI can never drift into
    different find-or-create/publish behavior for the same operation.

    Unlike publish-page's --page-id path (which only touches fields
    actually given, so a bare `--publish` can flip status without
    disturbing existing content), this always overwrites content/title --
    right for a caller that just rendered fresh content and wants it to
    win, wrong for an interactive "just change one field" edit.

    Before overwriting an *existing* page's content, checks it hasn't drifted from what wspy itself
    last wrote there (_check_no_drift()) -- raises WPContentDriftError instead of clobbering a human's
    hand-edit, unless force=True. After any successful create/update, records a fresh fingerprint
    (_record_publish_fingerprint()) so the next publish has a baseline to check against. Returns
    (page_dict, created_bool). Raises WPError (or the WPContentDriftError subclass) on any failure --
    the caller decides how to present that (CLI stderr vs. an HTML error panel)."""
    page = find_page(site_url, username, app_password, slug, parent)
    created = page is None
    if created:
        page = create_page(site_url, username, app_password, slug, title, parent,
                            content=content, status="draft")
    else:
        if not force:
            _check_no_drift(site_url, username, app_password, page["id"], page.get("link"))
        page = update_page(site_url, username, app_password, page["id"],
                            content=content, title=title)
    if do_publish:
        page = publish_page(site_url, username, app_password, page["id"])
    _record_publish_fingerprint(site_url, username, app_password, page["id"])
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


def publish_page_at_path(site_url, username, app_password, levels, content, do_publish=False,
                          stub_content=None, force=False):
    """Walk levels[:-1] top-down, creating each as a draft stub only if missing (never overwriting
    an existing page, e.g. a hand-created suite stub or a future wspy-testpoint aggregate) -- the
    same find-or-create logic find_or_create_page_path() already uses -- then publish_page_content()
    the final (leaf) level with real content. Neither existing primitive covers this alone:
    find_or_create_page_path() has no content parameter, and publish_page_content() only operates at
    one flat (slug, parent) level with no walk. Closes that gap (INVESTIGATION.md Tier 3 item 6) for
    callers that build a page at an exact hierarchy path, e.g. levels=[("cpu2026", "cpu2026"),
    ("706.stockfish_r", "706.stockfish_r")].

    stub_content, if given, is a {level_index: content} dict (0-based, matching `levels`) applied
    only when that parent level is newly *created* during the walk -- an already-existing page at
    that level is never touched, same as every other level. Default None means every stub is
    created empty, unchanged from this function's original behavior. Lets a caller give a
    freshly-created intermediate page (e.g. a machine-level stub) real content -- such as a link to
    its catalog entry -- without risking clobbering real content a later, separate publish already
    put there. force is passed straight through to the leaf's publish_page_content() call -- parent
    stub levels are never overwritten regardless (create-if-missing only), so drift protection only
    ever applies to the one page this call actually sets content on.

    Returns (page_dict, created_bool) for the leaf page only, same shape as publish_page_content()."""
    *parent_levels, (leaf_slug, leaf_title) = levels
    parent_id = 0
    stub_content = stub_content or {}
    for i, (slug, title) in enumerate(parent_levels):
        page = find_page(site_url, username, app_password, slug, parent_id)
        if page is None:
            page = create_page(site_url, username, app_password, slug, title, parent_id,
                                content=stub_content.get(i, ""))
        parent_id = page["id"]
    return publish_page_content(site_url, username, app_password, leaf_slug, parent_id, leaf_title,
                                 content, do_publish=do_publish, force=force)


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
