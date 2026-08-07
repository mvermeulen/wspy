<?php
/**
 * Plugin Name: wspy reference-matrix table controls
 * Description: Adds a lightweight, dependency-free search/sort/column-toggle toolbar above any
 * <table class="wspy-refmatrix"> on the page -- used by scripts/publish_reference_matrix.py in the
 * wspy repo (https://github.com/mvermeulen/wspy) to make the reference-matrix pages it publishes
 * navigable as the table grows. Ships as a site-wide plugin rather than inline JS/CSS in the
 * published page content because the wspy WordPress service account (web/wp_client.py) is
 * deliberately scoped without the unfiltered_html capability -- <script>/<style> embedded directly
 * in REST-published post content gets silently stripped by wp_kses_post() on save. This plugin's own
 * code isn't subject to that filter (it's trusted server-side code, not user-submitted post content),
 * so all the interactive behavior lives here instead; the generated pages only ever carry plain
 * <table>/<th>/<td> markup with data-col-kind attributes and a `title`-bearing info span, which pass
 * wp_kses_post() unchanged.
 *
 * Not part of the wspy codebase's own build/test -- install this file as a WordPress plugin on the
 * target site (Plugins -> Add New -> Upload Plugin, or drop into
 * wp-content/plugins/wspy-refmatrix-assets/ and activate), same as scripts/wp-auth-bridge.php.
 *
 * Column grouping: publish_reference_matrix.py tags each metric column data-col-kind="raw" (an
 * absolute accumulated count/duration, e.g. context switches, page faults) or "ratio" (already
 * normalized -- a percentage, a per-1000-instruction rate, IPC, bandwidth) per its own
 * RAW_COUNT_METRICS list. The toolbar's "show raw counts" checkbox starts unchecked, hiding
 * data-col-kind="raw" columns by default -- ratios are more comparable across differently-sized runs
 * and are what most readers want first.
 *
 * Tooltips: info-span `title` text comes from doc/METRICS.md's own per-metric one-line descriptions
 * (parsed by publish_reference_matrix.py's load_metric_descriptions()), so the browser's native
 * hover tooltip already works with zero JS; this plugin only adds the "ⓘ" affordance's cursor/opacity
 * styling so it reads as hoverable without spelling every description out in the table itself.
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
    display: flex; flex-wrap: wrap; gap: 1em; align-items: center;
    margin: 0 0 0.5em; font-size: 0.9em;
}
.wspy-refmatrix-toolbar input[type="search"] { font-size: inherit; padding: 0.2em 0.4em; }
table.wspy-refmatrix.wspy-hide-raw td[data-col-kind="raw"],
table.wspy-refmatrix.wspy-hide-raw th[data-col-kind="raw"] { display: none; }
table.wspy-refmatrix th { cursor: pointer; user-select: none; white-space: nowrap; }
table.wspy-refmatrix th.wspy-sorted-asc::after { content: " \25B2"; }
table.wspy-refmatrix th.wspy-sorted-desc::after { content: " \25BC"; }
table.wspy-refmatrix .wspy-info { cursor: help; opacity: 0.6; font-style: normal; }
table.wspy-refmatrix tr.wspy-row-hidden { display: none; }
</style>
<script>
(function () {
    function buildToolbar(table) {
        var toolbar = document.createElement('div');
        toolbar.className = 'wspy-refmatrix-toolbar';

        var search = document.createElement('input');
        search.type = 'search';
        search.placeholder = 'Filter rows…';
        search.addEventListener('input', function () {
            var q = search.value.trim().toLowerCase();
            Array.prototype.forEach.call(table.tBodies[0].rows, function (row) {
                var visible = q.length === 0 || row.textContent.toLowerCase().indexOf(q) !== -1;
                row.classList.toggle('wspy-row-hidden', !visible);
            });
        });
        toolbar.appendChild(search);

        if (table.querySelector('[data-col-kind="raw"]')) {
            var label = document.createElement('label');
            var toggle = document.createElement('input');
            toggle.type = 'checkbox';
            toggle.checked = false;
            table.classList.add('wspy-hide-raw');
            toggle.addEventListener('change', function () {
                table.classList.toggle('wspy-hide-raw', !toggle.checked);
            });
            label.appendChild(toggle);
            label.appendChild(document.createTextNode(' show raw counts'));
            toolbar.appendChild(label);
        }

        table.parentNode.insertBefore(toolbar, table);
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
        buildToolbar(table);
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
