<?php
/**
 * Plugin Name: wspy reference-matrix table controls
 * Description: Adds a lightweight, dependency-free search/sort/column-group-toggle/row-toggle
 * toolbar above any <table class="wspy-refmatrix"> on the page -- used by
 * scripts/publish_reference_matrix.py in the wspy repo (https://github.com/mvermeulen/wspy) to make
 * the reference-matrix pages it publishes navigable as the table grows. Ships as a site-wide plugin
 * rather than inline JS/CSS in the published page content because the wspy WordPress service account
 * (web/wp_client.py) is deliberately scoped without the unfiltered_html capability --
 * <script>/<style> embedded directly in REST-published post content gets silently stripped by
 * wp_kses_post() on save. This plugin's own code isn't subject to that filter (it's trusted
 * server-side code, not user-submitted post content), so all the interactive behavior lives here
 * instead; the generated pages only ever carry plain <table>/<th>/<td> markup with
 * data-col-kind/data-col-group attributes and a data-desc-bearing info span, all of which pass
 * wp_kses_post() unchanged (a bare `title` attribute would not -- see applyTooltips() below).
 *
 * Not part of the wspy codebase's own build/test -- install this file as a WordPress plugin on the
 * target site (Plugins -> Add New -> Upload Plugin, or drop into
 * wp-content/plugins/wspy-refmatrix-assets/ and activate), same as scripts/wp-auth-bridge.php.
 *
 * Column groups: publish_reference_matrix.py tags each metric column data-col-group="<token>" (the
 * --counters/--system/--power group that produces it, via joblib.resolve_column_group() plus its own
 * SUPPLEMENTARY_COLUMN_GROUPS for what that doesn't cover -- rusage, IBS, GPU, counter coverage) and
 * data-col-kind="raw"|"ratio" (an absolute accumulated count/duration vs. something already
 * normalized -- a percentage, a per-1000-instruction rate, IPC, bandwidth). The "Columns" panel below
 * shows one checkbox per group actually present in a given table, defaulting a group to unchecked
 * only if *every* metric in it is data-col-kind="raw" (e.g. rusage/software-counter groups) -- a
 * mixed group (e.g. branch, which has both the ratio "branch miss" and raw AMD/ARM extras) defaults
 * checked, since it still contains something worth seeing by default.
 *
 * Rows: one checkbox per row (test point), independent of and combined with the free-text search --
 * a row shows only if it matches the search text *and* its own checkbox is checked. "All"/"None"
 * buttons bulk-toggle the row checkboxes without touching the search box.
 *
 * Tooltips: info-span text comes from doc/METRICS.md's own per-metric one-line descriptions (parsed
 * by publish_reference_matrix.py's load_metric_descriptions()) -- see applyTooltips() for why it
 * ships as data-desc rather than a real title attribute.
 */

if (!defined('ABSPATH')) {
    exit;
}

add_action('wp_footer', 'wspy_refmatrix_assets_footer', 20);

function wspy_refmatrix_assets_footer() {
    if (!is_singular()) {
        return;
    }
    global $post;
    if (!$post || strpos($post->post_content, 'wspy-refmatrix') === false) {
        return;
    }
    ?>
<style>
table.wspy-refmatrix { margin-top: 0.5em; }
.wspy-refmatrix-toolbar {
    display: flex; flex-wrap: wrap; gap: 1em; align-items: flex-start;
    margin: 0 0 0.5em; font-size: 0.9em;
}
.wspy-refmatrix-toolbar input[type="search"] { font-size: inherit; padding: 0.2em 0.4em; }
.wspy-refmatrix-toolbar details {
    border: 1px solid rgba(127, 127, 127, 0.4); border-radius: 4px; padding: 0.3em 0.6em;
}
.wspy-refmatrix-toolbar summary { cursor: pointer; }
.wspy-refmatrix-toolbar details label { display: block; margin: 0.2em 0 0.2em 0.2em; }
.wspy-refmatrix-row-buttons { margin: 0.3em 0 0.3em 0.2em; }
.wspy-refmatrix-row-buttons button {
    font-size: 0.85em; margin-right: 0.4em; padding: 0.1em 0.5em; cursor: pointer;
}
table.wspy-refmatrix th { cursor: pointer; user-select: none; white-space: nowrap; }
table.wspy-refmatrix th.wspy-sorted-asc::after { content: " \25B2"; }
table.wspy-refmatrix th.wspy-sorted-desc::after { content: " \25BC"; }
table.wspy-refmatrix .wspy-info { cursor: help; opacity: 0.6; font-style: normal; }
table.wspy-refmatrix tr.wspy-row-search-miss,
table.wspy-refmatrix tr.wspy-row-unchecked { display: none; }
</style>
<script>
(function () {
    // Friendly labels for the --counters/--system/--power group tokens publish_reference_matrix.py
    // emits (joblib.py's ALL_GROUPS/"system"/"power" plus its own SUPPLEMENTARY_COLUMN_GROUPS) --
    // falls back to the raw token itself for anything this list hasn't caught up with yet, so a new
    // group never disappears from the panel, it just shows up unprettified.
    var GROUP_LABELS = {
        ipc: 'IPC', topdown: 'Topdown', 'topdown-frontend': 'Frontend/op-cache (AMD)',
        'topdown-backend': 'Backend deep-dive', 'topdown-optlb': 'Op-cache/TLB (AMD)',
        branch: 'Branch prediction', cache2: 'L2 cache', cache3: 'L3 cache', dcache: 'L1 dcache',
        icache: 'L1 icache', tlb: 'TLB', memory: 'Memory bandwidth', opcache: 'Op-cache',
        software: 'Software counters', float: 'Floating point', system: 'System', power: 'Power',
        process: 'Process/rusage', ibs: 'AMD IBS', gpu: 'GPU', coverage: 'Counter coverage',
        other: 'Other'
    };

    function buildSearchBox(toolbar, table) {
        var search = document.createElement('input');
        search.type = 'search';
        search.placeholder = 'Search rows…';
        search.addEventListener('input', function () {
            var q = search.value.trim().toLowerCase();
            Array.prototype.forEach.call(table.tBodies[0].rows, function (row) {
                var visible = q.length === 0 || row.textContent.toLowerCase().indexOf(q) !== -1;
                row.classList.toggle('wspy-row-search-miss', !visible);
            });
        });
        toolbar.appendChild(search);
    }

    function setGroupVisible(table, group, visible) {
        // The `hidden` attribute, not a CSS class -- group tokens are dynamic (whatever
        // publish_reference_matrix.py's metric_col_group() produced), so there's no fixed set of
        // data-col-group values a static stylesheet rule could target ahead of time the way the
        // fixed raw/ratio split could.
        Array.prototype.forEach.call(
            table.querySelectorAll('[data-col-group="' + group + '"]'),
            function (el) { el.hidden = !visible; }
        );
    }

    function buildColumnPanel(toolbar, table) {
        var groupKinds = {};  // group -> true iff every column seen so far in it is "raw"
        Array.prototype.forEach.call(table.querySelectorAll('th[data-col-group]'), function (th) {
            var g = th.getAttribute('data-col-group');
            var isRaw = th.getAttribute('data-col-kind') === 'raw';
            groupKinds[g] = (g in groupKinds) ? (groupKinds[g] && isRaw) : isRaw;
        });
        var groupNames = Object.keys(groupKinds).sort();
        if (groupNames.length < 2) {
            return;  // nothing to toggle between
        }

        var details = document.createElement('details');
        var summary = document.createElement('summary');
        summary.textContent = 'Columns';
        details.appendChild(summary);

        groupNames.forEach(function (g) {
            var checked = !groupKinds[g];  // start unchecked only if every column in it is raw
            setGroupVisible(table, g, checked);

            var label = document.createElement('label');
            var cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = checked;
            cb.addEventListener('change', function () { setGroupVisible(table, g, cb.checked); });
            label.appendChild(cb);
            label.appendChild(document.createTextNode(' ' + (GROUP_LABELS[g] || g)));
            details.appendChild(label);
        });

        toolbar.appendChild(details);
    }

    function buildRowPanel(toolbar, table) {
        var tbody = table.tBodies[0];
        if (!tbody || tbody.rows.length < 2) {
            return;  // nothing meaningful to select between
        }

        var details = document.createElement('details');
        var summary = document.createElement('summary');
        summary.textContent = 'Rows (' + tbody.rows.length + ')';
        details.appendChild(summary);

        var buttons = document.createElement('div');
        buttons.className = 'wspy-refmatrix-row-buttons';
        var allBtn = document.createElement('button');
        allBtn.type = 'button';
        allBtn.textContent = 'All';
        var noneBtn = document.createElement('button');
        noneBtn.type = 'button';
        noneBtn.textContent = 'None';
        buttons.appendChild(allBtn);
        buttons.appendChild(noneBtn);
        details.appendChild(buttons);

        var checkboxes = [];
        Array.prototype.forEach.call(tbody.rows, function (row) {
            var rowLabel = row.cells.length ? row.cells[0].textContent.trim() : row.textContent.trim();

            var label = document.createElement('label');
            var cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = true;
            cb.addEventListener('change', function () {
                row.classList.toggle('wspy-row-unchecked', !cb.checked);
            });
            label.appendChild(cb);
            label.appendChild(document.createTextNode(' ' + rowLabel));
            details.appendChild(label);
            checkboxes.push(cb);
        });

        function setAll(checked) {
            checkboxes.forEach(function (cb) {
                cb.checked = checked;
                cb.dispatchEvent(new Event('change'));
            });
        }
        allBtn.addEventListener('click', function () { setAll(true); });
        noneBtn.addEventListener('click', function () { setAll(false); });

        toolbar.appendChild(details);
    }

    function applyTooltips(table) {
        // The generated page carries the description as data-desc, not title -- WordPress's post-
        // content sanitizer allows data-* attributes by default but strips a bare title on <span>
        // for the wspy service account (no unfiltered_html; see publish_reference_matrix.py's
        // th() comment). Applying it as a real title here, client-side, never touches post content
        // at all, so it survives.
        Array.prototype.forEach.call(table.querySelectorAll('.wspy-info[data-desc]'), function (info) {
            info.title = info.getAttribute('data-desc');
            // Clicking the icon (e.g. to re-trigger a tooltip on touch) shouldn't also sort the
            // column its <th> click handler below is listening on.
            info.addEventListener('click', function (e) { e.stopPropagation(); });
        });
    }

    function sortByColumn(table, colIndex, th) {
        var tbody = table.tBodies[0];
        var rows = Array.prototype.slice.call(tbody.rows);
        var asc = !th.classList.contains('wspy-sorted-asc');

        Array.prototype.forEach.call(table.tHead.rows[0].cells, function (cell) {
            cell.classList.remove('wspy-sorted-asc', 'wspy-sorted-desc');
        });
        th.classList.add(asc ? 'wspy-sorted-asc' : 'wspy-sorted-desc');

        rows.sort(function (a, b) {
            var av = a.cells[colIndex] ? a.cells[colIndex].textContent.trim() : '';
            var bv = b.cells[colIndex] ? b.cells[colIndex].textContent.trim() : '';
            var an = parseFloat(av), bn = parseFloat(bv);
            var cmp = (!isNaN(an) && !isNaN(bn)) ? (an - bn) : av.localeCompare(bv);
            return asc ? cmp : -cmp;
        });
        rows.forEach(function (row) { tbody.appendChild(row); });
    }

    function setup(table) {
        var toolbar = document.createElement('div');
        toolbar.className = 'wspy-refmatrix-toolbar';
        buildSearchBox(toolbar, table);
        buildColumnPanel(toolbar, table);
        buildRowPanel(toolbar, table);
        table.parentNode.insertBefore(toolbar, table);

        applyTooltips(table);

        if (!table.tHead || !table.tHead.rows.length) {
            return;
        }
        Array.prototype.forEach.call(table.tHead.rows[0].cells, function (th, colIndex) {
            th.addEventListener('click', function () {
                sortByColumn(table, colIndex, th);
            });
        });
    }

    document.addEventListener('DOMContentLoaded', function () {
        Array.prototype.forEach.call(document.querySelectorAll('table.wspy-refmatrix'), setup);
    });
})();
</script>
    <?php
}
