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
 * buttons bulk-toggle the row checkboxes without touching the search box. When a row carries
 * data-row-group (cpu2026's intrate/intspeed/fprate/fpspeed benchmark-suite category,
 * publish_reference_matrix.py's row_group_for_test()) and at least two distinct categories are
 * present, the Rows panel organizes into the same tri-state group-checkbox hierarchy the Columns
 * panel uses -- pick a whole SPEC sub-suite (52 cpu2026 benchmarks across 4 categories) at once
 * instead of 52 individual clicks. Falls back to the original flat list when nothing has
 * data-row-group at all (every Phoronix page today).
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
.wspy-refmatrix-toolbar details label.wspy-refmatrix-child-toggle { margin-left: 1.4em; }
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
    // emits (joblib.py's ALL_GROUPS/"system"/"power" plus its own SUPPLEMENTARY_COLUMN_GROUPS, with
    // GROUP_ALIASES pre-merging topdown/topdown2/topdown-frontend/topdown-backend/topdown-optlb into
    // "topdown", cache2/cache3 into "cache", and "opcache" into "other" -- see that dict's own
    // comment) -- falls back to the raw token itself for anything this list hasn't caught up with
    // yet, so a new group never disappears from the panel, it just shows up unprettified.
    var GROUP_LABELS = {
        ipc: 'IPC', topdown: 'Topdown', branch: 'Branch prediction', cache: 'L2/L3 cache',
        dcache: 'L1 dcache', icache: 'L1 icache', tlb: 'TLB',
        memory: 'Memory bandwidth', software: 'Software counters',
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

    // Renders one tri-state group checkbox (all-checked/all-unchecked/indeterminate) plus one
    // labeled child checkbox per item into `container` -- shared by buildColumnPanel (items = the
    // columns in one data-col-group) and buildRowPanel (items = the rows in one data-row-group)
    // below, since both need the identical checking/unchecking-the-group-toggles-every-child,
    // checking/unchecking-a-child-updates-the-group tri-state mechanics. `itemLabel(item)` returns
    // one child's own display text, `applyFn(item, visible)` does the actual show/hide for one item,
    // `startChecked` is this group's initial state (every child starts there too). Returns the
    // child checkboxes, so a caller building an "All"/"None" button pair across several groups can
    // collect them all into one flat array.
    function buildTriStateGroup(container, groupLabelText, items, itemLabel, applyFn, startChecked) {
        var groupLabel = document.createElement('label');
        groupLabel.className = 'wspy-refmatrix-group-toggle';
        var groupCb = document.createElement('input');
        groupCb.type = 'checkbox';
        groupCb.checked = startChecked;
        groupLabel.appendChild(groupCb);
        var groupText = document.createElement('strong');
        groupText.textContent = groupLabelText + ' (' + items.length + ')';
        groupLabel.appendChild(groupText);
        container.appendChild(groupLabel);

        var childCheckboxes = items.map(function (item) {
            applyFn(item, startChecked);

            var label = document.createElement('label');
            label.className = 'wspy-refmatrix-child-toggle';
            var cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.checked = startChecked;
            cb.addEventListener('change', function () {
                applyFn(item, cb.checked);
                var checkedCount = childCheckboxes.filter(function (x) { return x.checked; }).length;
                groupCb.checked = checkedCount > 0;
                groupCb.indeterminate = checkedCount > 0 && checkedCount < childCheckboxes.length;
            });
            label.appendChild(cb);
            label.appendChild(document.createTextNode(' ' + itemLabel(item)));
            container.appendChild(label);
            return cb;
        });

        groupCb.addEventListener('change', function () {
            // Capture the target once -- each child's own "change" handler above recomputes and
            // overwrites groupCb.checked/indeterminate from the partial checked count as it fires,
            // so re-reading groupCb.checked live on every loop iteration (instead of this captured
            // snapshot) meant a sibling's handler could flip it back mid-loop: unchecking a
            // fully-checked group only ever cleared the first child before this fix, since
            // checkedCount stayed > 0 for every iteration except the last.
            var target = groupCb.checked;
            childCheckboxes.forEach(function (cb) {
                cb.checked = target;
                cb.dispatchEvent(new Event('change'));
            });
            // Children's handlers already leave the right value in the all-same-target case, but
            // pin it explicitly rather than trust the last child's handler to have been the final
            // write.
            groupCb.checked = target;
            groupCb.indeterminate = false;
        });

        return childCheckboxes;
    }

    // One checkbox per individual metric column (addresses "can't filter columns one by one"),
    // organized under a per-group header checkbox via buildTriStateGroup() above -- checking/
    // unchecking the group checkbox sets every column in it together; checking/unchecking an
    // individual column updates its own group checkbox's checked/indeterminate state to match, but
    // never touches any other group.
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
            buildTriStateGroup(
                details, GROUP_LABELS[g] || g, cols,
                function (c) { return c.metric; },
                function (c, visible) { setColumnVisible(table, c.colIndex, visible); },
                startChecked
            );
        });

        toolbar.appendChild(details);
    }

    // Friendly labels for cpu2026's own benchmark-suite categories (joblib.py's CPU2026_BENCHMARKS,
    // via publish_reference_matrix.py's row_group_for_test() -- the data-row-group attribute).
    // Falls back to the raw token for anything not listed, same convention GROUP_LABELS uses.
    var ROW_GROUP_LABELS = {
        intrate: 'SPECrate Integer', intspeed: 'SPECspeed Integer',
        fprate: 'SPECrate Floating Point', fpspeed: 'SPECspeed Floating Point'
    };
    var ROW_GROUP_ORDER = ['intrate', 'intspeed', 'fprate', 'fpspeed'];

    function rowLabelText(row) {
        return row.cells.length ? row.cells[0].textContent.trim() : row.textContent.trim();
    }

    function setRowVisible(row, visible) {
        row.classList.toggle('wspy-row-unchecked', !visible);
    }

    // Groups rows by data-row-group (e.g. cpu2026's intrate/intspeed/fprate/fpspeed) the same way
    // buildColumnPanel groups columns, via the shared buildTriStateGroup() helper -- one group
    // checkbox per category, one child checkbox per test point underneath it. Falls back to the
    // original flat, ungrouped list whenever fewer than two distinct buckets exist (a Phoronix page,
    // which has no such categorization at all; a cpu2026 page with everything in one category; or
    // any table too small to be worth a hierarchy), so this never regresses a page with nothing to
    // group by.
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

        var byGroup = {};
        var ungrouped = [];
        Array.prototype.forEach.call(tbody.rows, function (row) {
            var g = row.getAttribute('data-row-group');
            (g ? (byGroup[g] || (byGroup[g] = [])) : ungrouped).push(row);
        });
        var groupNames = Object.keys(byGroup).sort(function (a, b) {
            var ai = ROW_GROUP_ORDER.indexOf(a), bi = ROW_GROUP_ORDER.indexOf(b);
            if (ai === -1) ai = ROW_GROUP_ORDER.length;
            if (bi === -1) bi = ROW_GROUP_ORDER.length;
            return ai !== bi ? ai - bi : a.localeCompare(b);
        });

        var allCheckboxes = [];
        if (groupNames.length + (ungrouped.length ? 1 : 0) >= 2) {
            groupNames.forEach(function (g) {
                var cbs = buildTriStateGroup(
                    details, ROW_GROUP_LABELS[g] || g, byGroup[g], rowLabelText, setRowVisible, true);
                allCheckboxes = allCheckboxes.concat(cbs);
            });
            if (ungrouped.length) {
                var cbs = buildTriStateGroup(
                    details, 'Other', ungrouped, rowLabelText, setRowVisible, true);
                allCheckboxes = allCheckboxes.concat(cbs);
            }
        } else {
            Array.prototype.forEach.call(tbody.rows, function (row) {
                var label = document.createElement('label');
                var cb = document.createElement('input');
                cb.type = 'checkbox';
                cb.checked = true;
                cb.addEventListener('change', function () { setRowVisible(row, cb.checked); });
                label.appendChild(cb);
                label.appendChild(document.createTextNode(' ' + rowLabelText(row)));
                details.appendChild(label);
                allCheckboxes.push(cb);
            });
        }

        function setAll(checked) {
            allCheckboxes.forEach(function (cb) {
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
