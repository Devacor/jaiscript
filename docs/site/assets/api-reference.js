(function () {
	"use strict";
	var query = document.getElementById("api-query");
	var provider = document.getElementById("api-provider");
	var count = document.getElementById("api-count");
	if (!query || !provider || !count) { return; }
	var entries = Array.from(document.querySelectorAll(".api-entry"));
	var groups = Array.from(document.querySelectorAll(".api-group"));
	function filter() {
		var words = query.value.toLowerCase().trim().split(/\s+/).filter(Boolean);
		var visible = 0;
		entries.forEach(function (entry) {
			var matchesProvider = !provider.value || entry.dataset.provider === provider.value;
			var matchesText = words.every(function (word) { return entry.dataset.search.indexOf(word) !== -1; });
			entry.hidden = !(matchesProvider && matchesText);
			if (!entry.hidden) { visible += 1; }
		});
		groups.forEach(function (group) { group.hidden = !group.querySelector(".api-entry:not([hidden])"); });
		count.textContent = visible + " of " + entries.length + " callable signatures";
	}
	query.addEventListener("input", filter);
	provider.addEventListener("change", filter);
	filter();
}());
