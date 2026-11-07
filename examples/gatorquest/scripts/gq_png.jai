// Gator Quest - a minimal PNG writer (stored deflate blocks, CRC-32, Adler-32)
// so frame captures can be looked at as images. Pure script, no libraries.

import "gq_vid.jai";

array<int> CRC_TABLE;

void crc_init() {
	if (CRC_TABLE.size() == 256) { return; }
	CRC_TABLE = [];
	for (int n = 0; n < 256; ++n) {
		int c = n;
		for (int k = 0; k < 8; ++k) {
			if ((c & 1) != 0) { c = 0xEDB88320 ^ (c >> 1); }
			else { c = c >> 1; }
		}
		CRC_TABLE.push(c);
	}
}

int crc32(string s) {
	int c = 0xFFFFFFFF;
	int n = s.size();
	for (int i = 0; i < n; ++i) { c = CRC_TABLE[(c ^ (s[i] + 0)) & 255] ^ (c >> 8); }
	return c ^ 0xFFFFFFFF;
}

string png_chunk(string type, string data) {
	return be32(data.size()) + type + data + be32(crc32(type + data));
}

// the canvas scaled up by `scale`, 24-bit RGB
bool png_write_canvas(string path, int scale) {
	crc_init();
	int w = VW * scale;
	int h = VH * scale;
	string raw = "";
	for (int y = 0; y < VH; ++y) {
		string row = "" + to_char(0);
		for (int x = 0; x < VW; ++x) {
			int c = PIX[y * VW + x];
			string px = "" + to_char(PAL_R[c]) + to_char(PAL_G[c]) + to_char(PAL_B[c]);
			for (int s = 0; s < scale; ++s) { row += px; }
		}
		for (int s = 0; s < scale; ++s) { raw += row; }
	}
	// zlib stream of stored blocks
	string z = "" + to_char(0x78) + to_char(0x01);
	int pos = 0;
	int n = raw.size();
	while (pos < n) {
		int len = imin(65535, n - pos);
		bool last = pos + len >= n;
		z += "" + to_char(last ? 1 : 0) + to_char(len & 255) + to_char((len >> 8) & 255) + to_char((~len) & 255) + to_char(((~len) >> 8) & 255);
		z += raw.substr(pos, len);
		pos = pos + len;
	}
	int a = 1;
	int b = 0;
	for (int i = 0; i < n; ++i) {
		a = (a + (raw[i] + 0)) % 65521;
		b = (b + a) % 65521;
	}
	z += be32(b * 65536 + a);
	string png = "\x89PNG\r\n\x1a\n";
	png += png_chunk("IHDR", be32(w) + be32(h) + to_char(8) + to_char(2) + to_char(0) + to_char(0) + to_char(0));
	png += png_chunk("IDAT", z);
	png += png_chunk("IEND", "");
	return write_file(path, png);
}
