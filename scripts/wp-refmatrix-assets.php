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
.wspy-refmatrix-toolbar details label.wspy-refmatrix-group-toggle { margin-top: 0.6em; }
.wspy-refmatrix-toolbar details label.wspy-refmatrix-group-toggle:first-of-type { margin-top: 0.2em; }
.wspy-refmatrix-toolbar details label.wspy-refmatrix-col-toggle { margin-left: 1.4em; }
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
    // emits (joblib.py's ALL_GROUPS/"system"/"power" plus its own SUPPLEMENTARY_COLUMN_GROUPS,
    // topdown/topdown2/topdown-frontend/topdown-backend/topdown-optlb pre-merged into one "topdown"
    // token by metric_col_group() -- see that function's own comment) -- falls back to the raw token
    // itself for anything this list hasn't caught up with yet, so a new group never disappears from
    // the panel, it just shows up unprettified.
    var GROUP_LABELS = {
        ipc: 'IPC', topdown: 'Topdown', branch: 'Branch prediction', cache2: 'L2 cache',
        cache3: 'L3 cache', dcache: 'L1 dcache', icache: 'L1 icache', tlb: 'TLB',
        memory: 'Memory bandwidth', opcache: 'Op-cache', software: 'Software counters',
        float: 'Floating point', system: 'System', power: 'Power', process: 'Process/rusage',
        ibs: 'AMD IBS', gpu: 'GPU', coverage: 'Counter coverage', other: 'Other'
    };

    // Orders the Columns panel by publish_reference_matrix.py's own COLUMN_GROUP_ORDER (read off the
    // table's data-group-order attribute, so there's one source of truth rather than a second
    // hand-maintained copy here) -- fundamental groups (ipc, topdown) first, niche/vendor-specific
    // ones last. Any group not present in that list (shouldn't happen; the Python side has its own
    // test guarding against drift) sorts after everything that is listed, still before falling back
    // to alphabetical among themselves.
    function orderGroups(table, groupNames) {
        var order = (table.getAttribute('data-group-order') || '').split(',');
        return groupNames.slice().sort(function (a, b) {
            var ai = order.indexOf(a), bi = order.indexOf(b);
            if (ai === -1) ai = order.length;
            if (bi === -1) bi = order.length;
            return ai !== bi ? ai - bi : a.localeCompare(b);
        });
    }

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

    function setColumnVisible(table, colIndex, visible) {
        var headRow = table.tHead && table.tHead.rows[0];
        if (headRow && headRow.cells[colIndex]) {
            headRow.cells[colIndex].hidden = !visible;
        }
        Array.prototype.forEach.call(table.tBodies[0].rows, function (row) {
            if (row.cells[colIndex]) {
                row.cells[colIndex].hidden = !visible;
            }
        });
    }

    // One checkbox per individual metric column (addresses "can't filter columns one by one"),
    // organized under a per-group header checkbox that's a standard tri-state
    // all-checked/all-unchecked/indeterminate toggle for its own columns -- checking/unchecking the
    // group checkbox sets every column in it together; checking/unchecking an individual column
    // updates its own group checkbox's checked/indeterminate state to match, but never touches any
    // other group.
    function buildColumnPanel(toolbar, table) {
        var headRow = table.tHead && table.tHead.rows[0];
        if (!headRow) {
            return;
        }
        // group -> [{colIndex, metric, kind}], in column order (already alphabetical by metric --
        // build_machine_page() sorts `metrics` before emitting columns)
        var byGroup = {};
        Array.prototype.forEach.call(headRow.cells, function (th, colIndex) {
            var g = th.getAttribute('data-col-group');
            if (!g) {
                return;  // the leading "test point" label column carries no data-col-group
            }
            (byGroup[g] || (byGroup[g] = [])).push({
                colIndex: colIndex,
                metric: th.getAttribute('data-metric') || th.textContent.trim(),
                kind: th.getAttribute('data-col-kind')
            });
        });
        var groupNames = orderGroups(table, Object.keys(byGroup));
        if (groupNames.length < 2) {
            return;  // nothing to toggle between
        }

        var details = document.createElement('details');
        var summary = document.createElement('summary');
        summary.textContent = 'Columns';
        details.appendChild(summary);

        groupNames.forEach(function (g) {
            var cols = byGroup[g];
            var startChecked = cols.some(function (c) { return c.kind !== 'raw'; });

            var groupLabel = document.createElement('label');
            groupLabel.className = 'wspy-refmatrix-group-toggle';
            var groupCb = document.createElement('input');
            groupCb.type = 'checkbox';
            groupCb.checked = startChecked;
            groupLabel.appendChild(groupCb);
            var groupText = document.createElement('strong');
            groupText.textContent = GROUP_LABELS[g] || g;
            groupLabel.appendChild(groupText);
            details.appendChild(groupLabel);

            var colCheckboxes = cols.map(function (c) {
                setColumnVisible(table, c.colIndex, startChecked);

                var label = document.createElement('label');
                label.className = 'wspy-refmatrix-col-toggle';
                var cb = document.createElement('input');
                cb.type = 'checkbox';
                cb.checked = startChecked;
                cb.addEventListener('change', function () {
                    setColumnVisible(table, c.colIndex, cb.checked);
                    var checkedCount = colCheckboxes.filter(function (x) { return x.checked; }).length;
                    groupCb.checked = checkedCount > 0;
                    groupCb.indeterminate = checkedCount > 0 && checkedCount < colCheckboxes.length;
                });
                label.appendChild(cb);
                label.appendChild(document.createTextNode(' ' + c.metric));
                details.appendChild(label);
                return cb;
            });

            groupCb.addEventListener('change', function () {
                // Capture the target once -- each child's own "change" handler above recomputes
                // and overwrites groupCb.checked/indeterminate from the partial checked count as
                // it fires, so re-reading groupCb.checked live on every loop iteration (instead of
                // this captured snapshot) meant a sibling's handler could flip it back mid-loop:
                // unchecking a fully-checked group only ever cleared the first child before this
                // fix, since checkedCount stayed > 0 for every iteration except the last.
                var target = groupCb.checked;
                colCheckboxes.forEach(function (cb) {
                    cb.checked = target;
                    cb.dispatchEvent(new Event('change'));
                });
                // Children's handlers already leave the right value in the all-same-target case,
                // but pin it explicitly rather than trust the last child's handler to have been
                // the final write.
                groupCb.checked = target;
                groupCb.indeterminate = false;
            });
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
