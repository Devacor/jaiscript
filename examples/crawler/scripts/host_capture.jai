// The runner's read_key, captured before crawler.jai declares its own under that name. An import
// runs once per engine, so loading crawler.jai again in the same engine (the REPL) keeps the
// original instead of capturing the shim's read_key and recursing.
var HOST_READ_KEY_RAW = read_key;   // non-blocking: "" when no key is waiting
