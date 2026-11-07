// The guestbook's pages: each is a template under views/, rendered with template(path, names...). A template sees the names
// it is given and the program (when() below), and every ${...} it splices is HTML-escaped; a page another template made
// (a web::html) splices as it is. Edit a .spider file while the server runs and reload the page.

string when(int unix_seconds) { return time::date_from_unix(unix_seconds).format("${year}-${month:02}-${day:02} ${military_hours:02}:${minutes:02}"); }

web::html home_page(array<map<string, var>> entries, string name, string flash, int views) {
	string title = "Guestbook";
	web::html body = template("views/home.spider", entries, name, flash, views);
	return template("views/layout.spider", title, body);
}

web::html entry_page(map<string, var> entry) {
	string title = entry["name"] + " signed the guestbook";
	web::html body = template("views/entry.spider", entry);
	return template("views/layout.spider", title, body);
}
