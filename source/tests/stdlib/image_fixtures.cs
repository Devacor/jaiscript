// The fixture generator image_fixtures.ps1 compiles: see that file. Windows' codecs make and read the files, never JaiScript's.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Windows.Media;
using System.Windows.Media.Imaging;

public static class ImageFixtures {
	static StringBuilder arrays = new StringBuilder();
	static StringBuilder rows = new StringBuilder();
	static string dump = "";
	static uint seed = 1;
	static int count = 0;

	static uint Next() {
		seed = seed * 1664525u + 1013904223u;
		return seed >> 8;
	}

	static ulong Fnv(byte[] a_bytes) {
		ulong hash = 14695981039346656037UL;
		foreach (byte b in a_bytes) {
			hash ^= b;
			hash = unchecked(hash * 1099511628211UL);
		}
		return hash;
	}

	// Pixels as B, G, R, A rows: "gradient", "alpha" (a gradient whose alpha runs 0 to 255), "noise", "edges" (hard blocks
	// of saturated colour) or "photo" (smooth waves with a little noise, the shape of a photograph)
	public static byte[] Pattern(string a_kind, int a_width, int a_height) {
		byte[] p = new byte[a_width * a_height * 4];
		for (int y = 0; y < a_height; ++y) {
			for (int x = 0; x < a_width; ++x) {
				int i = (y * a_width + x) * 4;
				int r, g, b, a = 255;
				if (a_kind == "noise") {
					r = (int)(Next() & 255); g = (int)(Next() & 255); b = (int)(Next() & 255);
				} else if (a_kind == "edges") {
					int cell = ((x / 3) + (y / 2) * 3) % 8;
					r = (cell & 1) != 0 ? 255 : 0; g = (cell & 2) != 0 ? 255 : 0; b = (cell & 4) != 0 ? 255 : 0;
				} else if (a_kind == "photo") {
					double u = (double)x / a_width, v = (double)y / a_height;
					r = (int)(128 + 100 * Math.Sin(6.0 * u + 2.0 * v) + (Next() % 9) - 4);
					g = (int)(128 + 90 * Math.Cos(4.0 * v - 3.0 * u) + (Next() % 9) - 4);
					b = (int)(128 + 110 * Math.Sin(5.0 * (u + v)) + (Next() % 9) - 4);
					r = Math.Max(0, Math.Min(255, r)); g = Math.Max(0, Math.Min(255, g)); b = Math.Max(0, Math.Min(255, b));
				} else {
					r = a_width > 1 ? x * 255 / (a_width - 1) : 200;
					g = a_height > 1 ? y * 255 / (a_height - 1) : 100;
					b = (x * 7 + y * 13) & 255;
					if (a_kind == "alpha") { a = ((x + y) % 5 == 0) ? 0 : ((x * 37 + y * 11) & 255); if ((x + y) % 7 == 0) { a = 255; } }
				}
				p[i] = (byte)b; p[i + 1] = (byte)g; p[i + 2] = (byte)r; p[i + 3] = (byte)a;
			}
		}
		return p;
	}

	static byte[] Encode(BitmapEncoder a_encoder, BitmapSource a_source) {
		a_encoder.Frames.Add(BitmapFrame.Create(a_source));
		using (MemoryStream stream = new MemoryStream()) {
			a_encoder.Save(stream);
			return stream.ToArray();
		}
	}

	static BitmapSource FirstFrame(byte[] a_file) {
		BitmapDecoder decoder = BitmapDecoder.Create(new MemoryStream(a_file), BitmapCreateOptions.PreservePixelFormat | BitmapCreateOptions.IgnoreColorProfile, BitmapCacheOption.OnLoad);
		return decoder.Frames[0];
	}

	static byte Narrow(ushort a_value) { return (byte)((a_value * 255u + 32895u) >> 16); }

	// The file's pixels as R, G, B, A, by WIC. A 16-bit file's samples are read as they are stored and narrowed to the 8-bit
	// value nearest each (the rule the module documents), since WIC's own 16-bit conversions apply a gamma.
	public static byte[] ReferenceRgba(byte[] a_file, out int a_width, out int a_height) {
		BitmapSource frame = FirstFrame(a_file);
		a_width = frame.PixelWidth;
		a_height = frame.PixelHeight;
		byte[] rgba = new byte[a_width * a_height * 4];
		PixelFormat format = frame.Format;
		if (format == PixelFormats.Gray16 || format == PixelFormats.Rgb48 || format == PixelFormats.Rgba64) {
			int channels = format == PixelFormats.Gray16 ? 1 : format == PixelFormats.Rgb48 ? 3 : 4;
			ushort[] samples = new ushort[a_width * a_height * channels];
			frame.CopyPixels(samples, a_width * channels * 2, 0);
			for (int i = 0; i < a_width * a_height; ++i) {
				if (channels == 1) {
					rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = Narrow(samples[i]);
					rgba[4 * i + 3] = 255;
				} else {
					for (int c = 0; c < channels; ++c) { rgba[4 * i + c] = Narrow(samples[i * channels + c]); }
					if (channels == 3) { rgba[4 * i + 3] = 255; }
				}
			}
			return rgba;
		}
		FormatConvertedBitmap converted = new FormatConvertedBitmap(frame, PixelFormats.Bgra32, null, 0);
		converted.CopyPixels(rgba, a_width * 4, 0);
		for (int i = 0; i < rgba.Length; i += 4) {
			byte b = rgba[i];
			rgba[i] = rgba[i + 2];
			rgba[i + 2] = b;
		}
		return rgba;
	}

	static string Describe(byte[] a_file) {
		if (a_file.Length > 26 && a_file[1] == 'P') {
			return "png colour " + a_file[25] + " depth " + a_file[24] + (a_file[28] != 0 ? " interlaced" : "");
		}
		if (a_file.Length > 4 && a_file[0] == 0xff) {
			StringBuilder text = new StringBuilder("jpeg");
			int at = 2;
			while (at + 4 <= a_file.Length && a_file[at] == 0xff) {
				int marker = a_file[at + 1];
				int size = (a_file[at + 2] << 8) | a_file[at + 3];
				if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
					text.Append(" SOF" + (marker - 0xc0) + " components " + a_file[at + 9] + " sampling");
					for (int c = 0; c < a_file[at + 9]; ++c) { text.Append(" " + (a_file[at + 11 + 3 * c] >> 4) + "x" + (a_file[at + 11 + 3 * c] & 15)); }
				}
				if (marker == 0xdd) { text.Append(" restart " + ((a_file[at + 4] << 8) | a_file[at + 5])); }
				if (marker == 0xda) { break; }
				at += 2 + size;
			}
			return text.ToString();
		}
		return "gif";
	}

	static void AppendArray(string a_name, byte[] a_bytes) {
		arrays.Append("\tinline constexpr unsigned char " + a_name + "[] = {");
		for (int i = 0; i < a_bytes.Length; ++i) {
			if (i % 32 == 0) { arrays.Append("\n\t\t"); }
			arrays.Append(a_bytes[i].ToString());
			arrays.Append(',');
		}
		arrays.Append("\n\t};\n");
	}

	// One fixture: a file, and either the checksum of its reference pixels (a_exact) or the reference pixels themselves as a
	// PNG, for a decoder that is allowed a tolerance
	static void Add(string a_name, string a_format, byte[] a_file, byte[] a_rgba, int a_width, int a_height, bool a_exact) {
		AppendArray("k_" + a_name, a_file);
		string reference = "nullptr, 0";
		if (!a_exact) {
			byte[] bgra = (byte[])a_rgba.Clone();
			for (int i = 0; i < bgra.Length; i += 4) { byte t = bgra[i]; bgra[i] = bgra[i + 2]; bgra[i + 2] = t; }
			byte[] png = Encode(new PngBitmapEncoder(), BitmapSource.Create(a_width, a_height, 96, 96, PixelFormats.Bgra32, null, bgra, a_width * 4));
			AppendArray("k_" + a_name + "_reference", png);
			reference = "k_" + a_name + "_reference, sizeof(k_" + a_name + "_reference)";
		}
		rows.Append("\t\t{\"" + a_name + "\", \"" + a_format + "\", k_" + a_name + ", sizeof(k_" + a_name + "), " + a_width + ", " + a_height + ", 0x" + Fnv(a_rgba).ToString("x16") + "ull, " + reference + "},\n");
		Console.WriteLine(a_name + ": " + a_file.Length + " bytes, " + a_width + "x" + a_height + ", " + Describe(a_file));
		if (dump != "") {
			File.WriteAllBytes(Path.Combine(dump, a_name + "." + a_format), a_file);
			File.WriteAllBytes(Path.Combine(dump, a_name + ".rgba"), a_rgba);
		}
		++count;
	}

	static void AddPng(string a_name, BitmapSource a_source, bool a_interlaced) {
		PngBitmapEncoder encoder = new PngBitmapEncoder();
		encoder.Interlace = a_interlaced ? PngInterlaceOption.On : PngInterlaceOption.Off;
		byte[] file = Encode(encoder, a_source);
		int width, height;
		byte[] rgba = ReferenceRgba(file, out width, out height);
		Add(a_name, "png", file, rgba, width, height, true);
	}

	static BitmapSource Bgra(string a_kind, int a_width, int a_height) {
		return BitmapSource.Create(a_width, a_height, 96, 96, PixelFormats.Bgra32, null, Pattern(a_kind, a_width, a_height), a_width * 4);
	}

	static BitmapSource Converted(BitmapSource a_source, PixelFormat a_format) { return new FormatConvertedBitmap(a_source, a_format, null, 0); }

	static BitmapSource Packed(PixelFormat a_format, BitmapPalette a_palette, int a_width, int a_height, int a_bits) {
		int stride = (a_width * a_bits + 7) / 8;
		byte[] data = new byte[stride * a_height];
		for (int i = 0; i < data.Length; ++i) { data[i] = (byte)(Next() & 255); }
		return BitmapSource.Create(a_width, a_height, 96, 96, a_format, a_palette, data, stride);
	}

	static BitmapSource Deep(PixelFormat a_format, int a_width, int a_height, int a_channels) {
		ushort[] data = new ushort[a_width * a_height * a_channels];
		for (int i = 0; i < data.Length; ++i) { data[i] = (ushort)(Next() & 65535); }
		// some samples at the ends of the range and at the rounding boundaries
		data[0] = 0; data[1] = 65535; data[2] = 128; data[3] = 129; data[4] = 65407; data[5] = 65408;
		return BitmapSource.Create(a_width, a_height, 96, 96, a_format, null, data, a_width * a_channels * 2);
	}

	static BitmapPalette Palette(int a_count, bool a_alpha) {
		List<Color> colours = new List<Color>();
		for (int i = 0; i < a_count; ++i) {
			byte alpha = a_alpha ? (byte)(i % 4 == 0 ? 0 : i % 4 == 1 ? 255 : (Next() & 255)) : (byte)255;
			colours.Add(Color.FromArgb(alpha, (byte)(Next() & 255), (byte)(Next() & 255), (byte)(Next() & 255)));
		}
		return new BitmapPalette(colours);
	}

	static void Png() {
		AddPng("png_rgba_17x33", Bgra("alpha", 17, 33), false);
		AddPng("png_rgba_17x33_interlaced", Bgra("alpha", 17, 33), true);
		AddPng("png_rgba_1x1", Bgra("alpha", 1, 1), false);
		AddPng("png_rgba_1x1_interlaced", Bgra("gradient", 1, 1), true);
		AddPng("png_rgba_noise_24x24", Bgra("noise", 24, 24), false);
		AddPng("png_rgb_gradient_40x30", Converted(Bgra("gradient", 40, 30), PixelFormats.Bgr24), false);
		AddPng("png_rgb_edges_3x5_interlaced", Converted(Bgra("edges", 3, 5), PixelFormats.Bgr24), true);
		AddPng("png_rgb_noise_48x48", Converted(Bgra("noise", 48, 48), PixelFormats.Bgr24), false);
		AddPng("png_rgb_photo_64x48", Converted(Bgra("photo", 64, 48), PixelFormats.Bgr24), false);
		AddPng("png_grey8_17x33", Converted(Bgra("photo", 17, 33), PixelFormats.Gray8), false);
		AddPng("png_grey8_9x9_interlaced", Converted(Bgra("noise", 9, 9), PixelFormats.Gray8), true);
		AddPng("png_grey4_19x7", Packed(PixelFormats.Gray4, null, 19, 7, 4), false);
		AddPng("png_grey2_13x5", Packed(PixelFormats.Gray2, null, 13, 5, 2), false);
		AddPng("png_grey1_33x17", Packed(PixelFormats.BlackWhite, null, 33, 17, 1), false);
		AddPng("png_grey1_33x17_interlaced", Packed(PixelFormats.BlackWhite, null, 33, 17, 1), true);
		AddPng("png_grey16_9x7", Deep(PixelFormats.Gray16, 9, 7, 1), false);
		AddPng("png_rgb48_9x7", Deep(PixelFormats.Rgb48, 9, 7, 3), false);
		AddPng("png_rgba64_9x7", Deep(PixelFormats.Rgba64, 9, 7, 4), false);
		AddPng("png_rgba64_11x13_interlaced", Deep(PixelFormats.Rgba64, 11, 13, 4), true);
		AddPng("png_palette8_32x32", Packed(PixelFormats.Indexed8, Palette(256, false), 32, 32, 8), false);
		AddPng("png_palette8_alpha_21x10", Packed(PixelFormats.Indexed8, Palette(256, true), 21, 10, 8), false);
		AddPng("png_palette8_alpha_21x10_interlaced", Packed(PixelFormats.Indexed8, Palette(256, true), 21, 10, 8), true);
		AddPng("png_palette4_17x9", Packed(PixelFormats.Indexed4, Palette(16, false), 17, 9, 4), false);
		AddPng("png_palette4_alpha_17x9_interlaced", Packed(PixelFormats.Indexed4, Palette(16, true), 17, 9, 4), true);
		AddPng("png_palette2_13x5", Packed(PixelFormats.Indexed2, Palette(4, false), 13, 5, 2), false);
		AddPng("png_palette1_33x17", Packed(PixelFormats.Indexed1, Palette(2, false), 33, 17, 1), false);
	}

	// a_kind's pattern as R, G, B, A
	static byte[] RgbaPattern(string a_kind, int a_width, int a_height) {
		byte[] p = Pattern(a_kind, a_width, a_height);
		for (int i = 0; i < p.Length; i += 4) { byte b = p[i]; p[i] = p[i + 2]; p[i + 2] = b; }
		return p;
	}

	static void AddJpeg(string a_name, byte[] a_file) {
		int width, height;
		byte[] rgba = ReferenceRgba(a_file, out width, out height);
		Add(a_name, "jpeg", a_file, rgba, width, height, false);
	}

	static byte[] WicJpeg(BitmapSource a_source, int a_quality) {
		JpegBitmapEncoder encoder = new JpegBitmapEncoder();
		encoder.QualityLevel = a_quality;
		return Encode(encoder, a_source);
	}

	// GDI+'s JPEG encoder, a second Windows encoder with its own tables and layout
	static byte[] GdiJpeg(string a_kind, int a_width, int a_height, long a_quality) {
		byte[] rgba = RgbaPattern(a_kind, a_width, a_height);
		using (System.Drawing.Bitmap bitmap = new System.Drawing.Bitmap(a_width, a_height, System.Drawing.Imaging.PixelFormat.Format24bppRgb)) {
			for (int y = 0; y < a_height; ++y) {
				for (int x = 0; x < a_width; ++x) {
					int i = (y * a_width + x) * 4;
					bitmap.SetPixel(x, y, System.Drawing.Color.FromArgb(rgba[i], rgba[i + 1], rgba[i + 2]));
				}
			}
			System.Drawing.Imaging.ImageCodecInfo codec = null;
			foreach (System.Drawing.Imaging.ImageCodecInfo each in System.Drawing.Imaging.ImageCodecInfo.GetImageEncoders()) {
				if (each.MimeType == "image/jpeg") { codec = each; }
			}
			System.Drawing.Imaging.EncoderParameters parameters = new System.Drawing.Imaging.EncoderParameters(1);
			parameters.Param[0] = new System.Drawing.Imaging.EncoderParameter(System.Drawing.Imaging.Encoder.Quality, a_quality);
			using (MemoryStream stream = new MemoryStream()) {
				bitmap.Save(stream, codec, parameters);
				return stream.ToArray();
			}
		}
	}

	static JpegShape Shape(int a_h0, int a_v0, int a_h1, int a_v1) {
		JpegShape shape = new JpegShape();
		shape.H = new int[] {a_h0, a_h1, a_h1};
		shape.V = new int[] {a_v0, a_v1, a_v1};
		return shape;
	}

	static void Jpeg(string a_extra) {
		AddJpeg("jpeg_wic_photo_64x48", WicJpeg(Converted(Bgra("photo", 64, 48), PixelFormats.Bgr24), 85));
		AddJpeg("jpeg_wic_gradient_40x30", WicJpeg(Converted(Bgra("gradient", 40, 30), PixelFormats.Bgr24), 90));
		AddJpeg("jpeg_wic_noise_24x24", WicJpeg(Converted(Bgra("noise", 24, 24), PixelFormats.Bgr24), 75));
		AddJpeg("jpeg_wic_edges_17x33", WicJpeg(Converted(Bgra("edges", 17, 33), PixelFormats.Bgr24), 95));
		AddJpeg("jpeg_wic_1x1", WicJpeg(Converted(Bgra("gradient", 1, 1), PixelFormats.Bgr24), 90));
		AddJpeg("jpeg_wic_3x5", WicJpeg(Converted(Bgra("edges", 3, 5), PixelFormats.Bgr24), 90));
		AddJpeg("jpeg_wic_grey_17x33", WicJpeg(Converted(Bgra("photo", 17, 33), PixelFormats.Gray8), 90));
		AddJpeg("jpeg_gdi_photo_48x40", GdiJpeg("photo", 48, 40, 80));
		AddJpeg("jpeg_gdi_edges_13x7", GdiJpeg("edges", 13, 7, 90));

		JpegShape s444 = Shape(1, 1, 1, 1);
		AddJpeg("jpeg_444_photo_33x17", JpegWriter.Write(RgbaPattern("photo", 33, 17), 33, 17, s444));
		AddJpeg("jpeg_422_photo_37x21", JpegWriter.Write(RgbaPattern("photo", 37, 21), 37, 21, Shape(2, 1, 1, 1)));
		AddJpeg("jpeg_440_photo_21x37", JpegWriter.Write(RgbaPattern("photo", 21, 37), 21, 37, Shape(1, 2, 1, 1)));
		AddJpeg("jpeg_411_photo_45x13", JpegWriter.Write(RgbaPattern("photo", 45, 13), 45, 13, Shape(4, 1, 1, 1)));
		AddJpeg("jpeg_420_edges_2x3", JpegWriter.Write(RgbaPattern("edges", 2, 3), 2, 3, Shape(2, 2, 1, 1)));
		JpegShape restart = Shape(2, 2, 1, 1);
		restart.Restart = 3;
		AddJpeg("jpeg_420_restart_35x29", JpegWriter.Write(RgbaPattern("photo", 35, 29), 35, 29, restart));
		JpegShape scans = Shape(2, 2, 1, 1);
		scans.Restart = 1;
		scans.SeparateScans = true;
		scans.RedefineTables = true;
		AddJpeg("jpeg_420_scans_redefined_29x23", JpegWriter.Write(RgbaPattern("photo", 29, 23), 29, 23, scans));
		JpegShape grey = Shape(1, 1, 1, 1);
		grey.Grey = true;
		grey.Restart = 2;
		AddJpeg("jpeg_grey_restart_19x11", JpegWriter.Write(RgbaPattern("gradient", 19, 11), 19, 11, grey));
		JpegShape adobe = Shape(1, 1, 1, 1);
		adobe.AdobeRgb = true;
		AddJpeg("jpeg_adobe_rgb_17x9", JpegWriter.Write(RgbaPattern("edges", 17, 9), 17, 9, adobe));
		JpegShape wide = Shape(2, 1, 1, 1);
		wide.Wide = true;
		AddJpeg("jpeg_extended_16bit_tables_15x9", JpegWriter.Write(RgbaPattern("photo", 15, 9), 15, 9, wide));
		foreach (int orientation in new int[] {3, 6, 8}) {
			JpegShape turned = Shape(2, 2, 1, 1);
			turned.Orientation = orientation;
			turned.BigEndianExif = orientation == 8;
			AddJpeg("jpeg_exif_orientation_" + orientation + "_9x5", JpegWriter.Write(RgbaPattern("gradient", 9, 5), 9, 5, turned));
		}
		JpegShape progressive = Shape(2, 2, 1, 1);
		progressive.Progressive = true;
		progressive.Approximation = true;
		AddJpeg("jpeg_progressive_approximation_40x30", JpegWriter.Write(RgbaPattern("photo", 40, 30), 40, 30, progressive));
		JpegShape progressive_grey = Shape(1, 1, 1, 1);
		progressive_grey.Grey = true;
		progressive_grey.Progressive = true;
		progressive_grey.Approximation = true;
		AddJpeg("jpeg_progressive_grey_21x13", JpegWriter.Write(RgbaPattern("noise", 21, 13), 21, 13, progressive_grey));
		JpegShape runs = Shape(1, 1, 1, 1);
		runs.Progressive = true;
		runs.EobRuns = true;
		AddJpeg("jpeg_progressive_eob_runs_33x25", JpegWriter.Write(RgbaPattern("gradient", 33, 25), 33, 25, runs));
		JpegShape progressive_restart = Shape(2, 1, 1, 1);
		progressive_restart.Progressive = true;
		progressive_restart.Approximation = true;
		progressive_restart.EobRuns = true;
		progressive_restart.Restart = 2;
		AddJpeg("jpeg_progressive_restart_27x19", JpegWriter.Write(RgbaPattern("photo", 27, 19), 27, 19, progressive_restart));
		// files the caller supplies (a photograph from a camera or an editor), each embedded as it is
		if (a_extra != "") {
			foreach (string path in Directory.GetFiles(a_extra, "*.jpg")) { AddJpeg("jpeg_file_" + Path.GetFileNameWithoutExtension(path), File.ReadAllBytes(path)); }
		}
	}

	// A GIF's reference: its first frame as Windows decodes it, laid on a transparent screen at the frame's offset, with every
	// transparent pixel's colour zeroed (the module's convention for a pixel it does not paint)
	static void AddGif(string a_name, byte[] a_file, int a_left, int a_top, int a_screenWidth, int a_screenHeight) {
		int width, height;
		byte[] frame = ReferenceRgba(a_file, out width, out height);
		int screenWidth = a_screenWidth != 0 ? a_screenWidth : width, screenHeight = a_screenHeight != 0 ? a_screenHeight : height;
		byte[] rgba = new byte[screenWidth * screenHeight * 4];
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				int sx = a_left + x, sy = a_top + y;
				if (sx >= screenWidth || sy >= screenHeight) { continue; }
				int from = (y * width + x) * 4, to = (sy * screenWidth + sx) * 4;
				if (frame[from + 3] == 0) { continue; }
				Array.Copy(frame, from, rgba, to, 4);
			}
		}
		Add(a_name, "gif", a_file, rgba, screenWidth, screenHeight, true);
	}

	static byte[] WicGif(BitmapSource a_source) { return Encode(new GifBitmapEncoder(), a_source); }

	static byte[] GdiGif(string a_kind, int a_width, int a_height) {
		byte[] rgba = RgbaPattern(a_kind, a_width, a_height);
		using (System.Drawing.Bitmap bitmap = new System.Drawing.Bitmap(a_width, a_height, System.Drawing.Imaging.PixelFormat.Format24bppRgb)) {
			for (int y = 0; y < a_height; ++y) {
				for (int x = 0; x < a_width; ++x) {
					int i = (y * a_width + x) * 4;
					bitmap.SetPixel(x, y, System.Drawing.Color.FromArgb(rgba[i], rgba[i + 1], rgba[i + 2]));
				}
			}
			using (MemoryStream stream = new MemoryStream()) {
				bitmap.Save(stream, System.Drawing.Imaging.ImageFormat.Gif);
				return stream.ToArray();
			}
		}
	}

	static byte[] RandomIndexes(int a_count, int a_colours) {
		byte[] indexes = new byte[a_count];
		for (int i = 0; i < a_count; ++i) { indexes[i] = (byte)(Next() % (uint)a_colours); }
		return indexes;
	}

	static byte[] RandomPalette(int a_colours) {
		byte[] palette = new byte[a_colours * 3];
		for (int i = 0; i < palette.Length; ++i) { palette[i] = (byte)(Next() & 255); }
		return palette;
	}

	static void Gif() {
		AddGif("gif_wic_palette8_32x32", WicGif(Packed(PixelFormats.Indexed8, Palette(256, false), 32, 32, 8)), 0, 0, 0, 0);
		AddGif("gif_wic_palette4_alpha_17x9", WicGif(Packed(PixelFormats.Indexed4, Palette(16, true), 17, 9, 4)), 0, 0, 0, 0);
		AddGif("gif_wic_photo_24x24", WicGif(Bgra("photo", 24, 24)), 0, 0, 0, 0);
		AddGif("gif_wic_1x1", WicGif(Packed(PixelFormats.Indexed1, Palette(2, false), 1, 1, 1)), 0, 0, 0, 0);
		AddGif("gif_gdi_edges_19x13", GdiGif("edges", 19, 13), 0, 0, 0, 0);

		GifShape interlaced = new GifShape();
		interlaced.Interlaced = true;
		AddGif("gif_interlaced_13x11", GifWriter.Write(RandomIndexes(13 * 11, 16), 13, 11, RandomPalette(16), interlaced), 0, 0, 0, 0);
		GifShape offset = new GifShape();
		offset.ScreenWidth = 20; offset.ScreenHeight = 15; offset.Left = 3; offset.Top = 2;
		offset.Transparent = 1;
		offset.LocalTable = true;
		AddGif("gif_frame_offset_transparent_20x15", GifWriter.Write(RandomIndexes(10 * 8, 4), 10, 8, RandomPalette(4), offset), 3, 2, 20, 15);
		GifShape clipped = new GifShape();
		clipped.ScreenWidth = 9; clipped.ScreenHeight = 7; clipped.Left = 4; clipped.Top = 3;
		AddGif("gif_frame_past_the_screen_9x7", GifWriter.Write(RandomIndexes(8 * 8, 8), 8, 8, RandomPalette(8), clipped), 4, 3, 9, 7);
		GifShape full = new GifShape();
		full.ClearWhenFull = false;
		AddGif("gif_table_full_no_clear_90x80", GifWriter.Write(RandomIndexes(90 * 80, 256), 90, 80, RandomPalette(256), full), 0, 0, 0, 0);
		GifShape cleared = new GifShape();
		cleared.ClearWhenFull = true;
		AddGif("gif_table_full_cleared_90x80", GifWriter.Write(RandomIndexes(90 * 80, 256), 90, 80, RandomPalette(256), cleared), 0, 0, 0, 0);
		GifShape small = new GifShape();
		small.Minimum = 2;
		small.Old = true;
		small.SecondFrame = true;
		AddGif("gif87a_two_frames_7x5", GifWriter.Write(RandomIndexes(7 * 5, 3), 7, 5, RandomPalette(3), small), 0, 0, 0, 0);
		GifShape wide = new GifShape();
		wide.Minimum = 8;
		AddGif("gif_minimum_8_bits_two_colours_6x4", GifWriter.Write(RandomIndexes(6 * 4, 2), 6, 4, RandomPalette(2), wide), 0, 0, 0, 0);
	}

	public static void Run(string a_header, string a_dump, string a_extra) {
		dump = a_dump;
		if (dump != "") { Directory.CreateDirectory(dump); }
		Png();
		Jpeg(a_extra);
		Gif();
		StringBuilder text = new StringBuilder();
		text.Append("// Generated by image_fixtures.ps1: do not edit. Each row is a file (from Windows' encoders, or the generator's own JPEG writer\n");
		text.Append("// for the shapes they do not write) and the pixels Windows' decoder reads from it: the FNV-1a hash of its R, G, B, A bytes,\n");
		text.Append("// and for a lossy file the pixels themselves, as a PNG.\n");
		text.Append("#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\nnamespace jai {\nnamespace image_fixtures {\n\n");
		text.Append("\tstruct fixture {\n\t\tconst char* name;\n\t\tconst char* format;\n\t\tconst unsigned char* bytes;\n\t\tsize_t size;\n\t\tuint32_t width;\n\t\tuint32_t height;\n\t\tuint64_t rgba_hash;\n\t\tconst unsigned char* reference;\n\t\tsize_t reference_size;\n\t};\n\n");
		text.Append(arrays);
		text.Append("\n\tinline constexpr fixture k_fixtures[] = {\n");
		text.Append(rows);
		text.Append("\t};\n\n} // namespace image_fixtures\n} // namespace jai\n");
		File.WriteAllText(a_header, text.ToString(), new UTF8Encoding(false));
		Console.WriteLine(count + " fixtures, " + new FileInfo(a_header).Length + " bytes of header");
	}
}

// A JPEG writer for the shapes Windows' encoders do not write: any sampling factors, restart intervals, one scan per
// component with tables redefined between them, 16-bit quantization tables, EXIF orientation in either byte order, Adobe
// RGB, and progressive scans with spectral selection, successive approximation and EOB runs. Windows' decoder reads each
// file for its reference pixels, so the writer needs only to be valid, and a file it got wrong fails at generation.
public class JpegShape {
	public int[] H = {2, 1, 1};
	public int[] V = {2, 1, 1};
	public bool Grey;
	public bool AdobeRgb;
	public int Restart;
	public bool SeparateScans;
	public bool RedefineTables;
	public bool Wide;
	public int Orientation;
	public bool BigEndianExif;
	public bool Progressive;
	public bool Approximation;
	public bool EobRuns;
	public int Quality = 90;
}

public static class JpegWriter {
	static readonly int[] Zigzag = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
		35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
	// T.81 Annex K.1, in natural order
	static readonly int[] Luma = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
		18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
	static readonly int[] Chroma = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
		99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

	class Plane { public int Width, Height, BlocksAcross, BlocksDown, H, V, Table; public int[][] Blocks; }

	// Huffman tables the writer needs no statistics for: every DC category at 4 bits, every AC symbol at 8 (incomplete, so no
	// code is all ones)
	static byte[] dcCounts = new byte[16];
	static byte[] dcValues;
	static byte[] acCounts = new byte[16];
	static byte[] acValues;
	static int[] dcCode = new int[256], dcLength = new int[256], acCode = new int[256], acLength = new int[256];

	static void Tables() {
		dcCounts[3] = 12;
		dcValues = new byte[12];
		for (int i = 0; i < 12; ++i) { dcValues[i] = (byte)i; dcCode[i] = i; dcLength[i] = 4; }
		List<byte> ac = new List<byte>();
		for (int run = 0; run < 16; ++run) {
			ac.Add((byte)(run << 4));
			for (int size = 1; size <= 10; ++size) { ac.Add((byte)((run << 4) | size)); }
		}
		acValues = ac.ToArray();
		acCounts[7] = (byte)acValues.Length;
		for (int i = 0; i < acValues.Length; ++i) { acCode[acValues[i]] = i; acLength[acValues[i]] = 8; }
	}

	class Bits {
		public List<byte> Out = new List<byte>();
		int buffer, used;
		public void Put(int a_value, int a_count) {
			for (int i = a_count - 1; i >= 0; --i) {
				buffer = (buffer << 1) | ((a_value >> i) & 1);
				if (++used == 8) { Byte((byte)buffer); buffer = 0; used = 0; }
			}
		}
		void Byte(byte a_byte) { Out.Add(a_byte); if (a_byte == 0xff) { Out.Add(0); } }
		public void Flush() { while (used != 0) { Put(1, 1); } }
		public void Marker(int a_marker) { Flush(); Out.Add(0xff); Out.Add((byte)a_marker); }
	}

	static int BitLength(int a_value) { int n = 0; while (a_value != 0) { ++n; a_value >>= 1; } return n; }
	static void Dc(Bits a_bits, int a_diff) {
		int magnitude = Math.Abs(a_diff), size = BitLength(magnitude);
		a_bits.Put(dcCode[size], dcLength[size]);
		if (size > 0) { a_bits.Put(a_diff < 0 ? a_diff - 1 : a_diff, size); }
	}
	static void Ac(Bits a_bits, int a_symbol) { a_bits.Put(acCode[a_symbol], acLength[a_symbol]); }
	static void AcValue(Bits a_bits, int a_run, int a_value) {
		int size = BitLength(Math.Abs(a_value));
		Ac(a_bits, (a_run << 4) | size);
		a_bits.Put(a_value < 0 ? a_value - 1 : a_value, size);
	}

	static List<byte> Segment(int a_marker, List<byte> a_body) {
		List<byte> s = new List<byte> {0xff, (byte)a_marker, (byte)((a_body.Count + 2) >> 8), (byte)(a_body.Count + 2)};
		s.AddRange(a_body);
		return s;
	}

	static int[] Scaled(int[] a_table, int a_quality) {
		int scale = a_quality < 50 ? 5000 / a_quality : 200 - 2 * a_quality;
		int[] result = new int[64];
		for (int i = 0; i < 64; ++i) { result[i] = Math.Max(1, Math.Min(255, (a_table[i] * scale + 50) / 100)); }
		return result;
	}

	static List<byte> Quantization(int a_slot, int[] a_table, bool a_wide) {
		List<byte> body = new List<byte> {(byte)((a_wide ? 0x10 : 0) | a_slot)};
		for (int k = 0; k < 64; ++k) {
			int q = a_table[Zigzag[k]];
			if (a_wide) { body.Add((byte)(q >> 8)); }
			body.Add((byte)q);
		}
		return body;
	}

	static List<byte> Huffman(int a_slot) {
		List<byte> body = new List<byte> {(byte)a_slot};
		body.AddRange(dcCounts); body.AddRange(dcValues);
		body.Add((byte)(0x10 | a_slot));
		body.AddRange(acCounts); body.AddRange(acValues);
		return body;
	}

	// a_rgba as a JPEG of a_shape
	public static byte[] Write(byte[] a_rgba, int a_width, int a_height, JpegShape a_shape) {
		Tables();
		int count = a_shape.Grey ? 1 : 3;
		int hMax = 1, vMax = 1;
		for (int c = 0; c < count; ++c) { hMax = Math.Max(hMax, a_shape.H[c]); vMax = Math.Max(vMax, a_shape.V[c]); }
		int mcusAcross = (a_width + 8 * hMax - 1) / (8 * hMax), mcusDown = (a_height + 8 * vMax - 1) / (8 * vMax);
		int[] lumaTable = Scaled(Luma, a_shape.Quality), chromaTable = Scaled(Chroma, a_shape.Quality);
		if (a_shape.Wide) { for (int i = 0; i < 64; ++i) { lumaTable[i] = lumaTable[i] * 3 + 256; } }
		Plane[] planes = new Plane[count];
		for (int c = 0; c < count; ++c) {
			Plane p = new Plane();
			p.H = a_shape.H[c]; p.V = a_shape.V[c];
			p.Width = (a_width * p.H + hMax - 1) / hMax; p.Height = (a_height * p.V + vMax - 1) / vMax;
			p.BlocksAcross = mcusAcross * p.H; p.BlocksDown = mcusDown * p.V;
			p.Table = c == 0 || a_shape.AdobeRgb ? 0 : 1;
			int sx = hMax / p.H, sy = vMax / p.V;
			// the component's samples: box-averaged over the pixels each covers, the edges repeated out to whole blocks
			double[] samples = new double[p.BlocksAcross * 8 * p.BlocksDown * 8];
			for (int y = 0; y < p.BlocksDown * 8; ++y) {
				for (int x = 0; x < p.BlocksAcross * 8; ++x) {
					int cx = Math.Min(x, p.Width - 1), cy = Math.Min(y, p.Height - 1);
					double sum = 0; int n = 0;
					for (int dy = 0; dy < sy; ++dy) {
						for (int dx = 0; dx < sx; ++dx) {
							int px = Math.Min(cx * sx + dx, a_width - 1), py = Math.Min(cy * sy + dy, a_height - 1);
							int i = (py * a_width + px) * 4;
							double r = a_rgba[i], g = a_rgba[i + 1], b = a_rgba[i + 2];
							double value;
							if (a_shape.AdobeRgb) { value = c == 0 ? r : c == 1 ? g : b; }
							else if (c == 0) { value = 0.299 * r + 0.587 * g + 0.114 * b; }
							else if (c == 1) { value = -0.168736 * r - 0.331264 * g + 0.5 * b + 128; }
							else { value = 0.5 * r - 0.418688 * g - 0.081312 * b + 128; }
							sum += value; ++n;
						}
					}
					samples[y * p.BlocksAcross * 8 + x] = sum / n;
				}
			}
			int[] table = p.Table == 0 ? lumaTable : chromaTable;
			p.Blocks = new int[p.BlocksAcross * p.BlocksDown][];
			for (int by = 0; by < p.BlocksDown; ++by) {
				for (int bx = 0; bx < p.BlocksAcross; ++bx) {
					int[] coefficients = new int[64];
					for (int v = 0; v < 8; ++v) {
						for (int u = 0; u < 8; ++u) {
							double sum = 0;
							for (int y = 0; y < 8; ++y) {
								for (int x = 0; x < 8; ++x) {
									sum += (samples[(by * 8 + y) * p.BlocksAcross * 8 + bx * 8 + x] - 128) * Math.Cos((2 * x + 1) * u * Math.PI / 16) * Math.Cos((2 * y + 1) * v * Math.PI / 16);
								}
							}
							double cu = u == 0 ? Math.Sqrt(0.5) : 1, cv = v == 0 ? Math.Sqrt(0.5) : 1;
							coefficients[v * 8 + u] = (int)Math.Round(0.25 * cu * cv * sum / table[v * 8 + u]);
						}
					}
					p.Blocks[by * p.BlocksAcross + bx] = coefficients;
				}
			}
			planes[c] = p;
		}

		List<byte> file = new List<byte> {0xff, 0xd8};
		if (!a_shape.AdobeRgb) { file.AddRange(Segment(0xe0, new List<byte> {(byte)'J', (byte)'F', (byte)'I', (byte)'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0})); }
		if (a_shape.Orientation != 0) {
			bool big = a_shape.BigEndianExif;
			List<byte> exif = new List<byte> {(byte)'E', (byte)'x', (byte)'i', (byte)'f', 0, 0};
			Action<int> u16 = v => { if (big) { exif.Add((byte)(v >> 8)); exif.Add((byte)v); } else { exif.Add((byte)v); exif.Add((byte)(v >> 8)); } };
			Action<int> u32 = v => { if (big) { u16(v >> 16); u16(v & 0xffff); } else { u16(v & 0xffff); u16(v >> 16); } };
			exif.Add((byte)(big ? 'M' : 'I')); exif.Add((byte)(big ? 'M' : 'I'));
			u16(42); u32(8);
			// IDF0: an XResolution-like entry the reader skips, then the orientation
			u16(2);
			u16(0x011a); u16(5); u32(1); u32(38);
			u16(0x0112); u16(3); u32(1); u16(a_shape.Orientation); u16(0);
			u32(0);
			u32(72); u32(1);
			file.AddRange(Segment(0xe1, exif));
		}
		if (a_shape.AdobeRgb) { file.AddRange(Segment(0xee, new List<byte> {(byte)'A', (byte)'d', (byte)'o', (byte)'b', (byte)'e', 0, 100, 0, 0, 0, 0, 0})); }
		List<byte> quantization = Quantization(0, lumaTable, a_shape.Wide);
		if (!a_shape.Grey && !a_shape.AdobeRgb && !a_shape.RedefineTables) { quantization.AddRange(Quantization(1, chromaTable, false)); }
		file.AddRange(Segment(0xdb, quantization));
		List<byte> frame = new List<byte> {8, (byte)(a_height >> 8), (byte)a_height, (byte)(a_width >> 8), (byte)a_width, (byte)count};
		for (int c = 0; c < count; ++c) {
			// with the tables redefined, every component names table 0 and the chroma scans come after a DQT that changes it
			int table = a_shape.RedefineTables ? 0 : planes[c].Table;
			frame.Add((byte)(c + 1)); frame.Add((byte)((planes[c].H << 4) | planes[c].V)); frame.Add((byte)table);
		}
		file.AddRange(Segment(a_shape.Progressive ? 0xc2 : a_shape.Wide ? 0xc1 : 0xc0, frame));
		List<byte> huffman = Huffman(0);
		file.AddRange(Segment(0xc4, huffman));
		if (a_shape.Restart != 0) { file.AddRange(Segment(0xdd, new List<byte> {(byte)(a_shape.Restart >> 8), (byte)a_shape.Restart})); }

		List<int[]> scans = new List<int[]>();   // components..., then Ss, Se, Ah, Al as the last four
		if (!a_shape.Progressive) {
			if (a_shape.SeparateScans) { for (int c = 0; c < count; ++c) { scans.Add(new int[] {c, 0, 63, 0, 0}); } }
			else { int[] all = new int[count + 4]; for (int c = 0; c < count; ++c) { all[c] = c; } all[count] = 0; all[count + 1] = 63; scans.Add(all); }
		} else {
			int dcLow = a_shape.Approximation ? 1 : 0;
			int[] dc = new int[count + 4]; for (int c = 0; c < count; ++c) { dc[c] = c; } dc[count + 3] = dcLow; scans.Add(dc);
			int high = a_shape.Approximation ? 2 : 0;
			scans.Add(new int[] {0, 1, 5, 0, high});
			for (int c = 1; c < count; ++c) { scans.Add(new int[] {c, 1, 63, 0, a_shape.Approximation ? 1 : 0}); }
			scans.Add(new int[] {0, 6, 63, 0, high});
			if (a_shape.Approximation) {
				scans.Add(new int[] {0, 1, 63, 2, 1});
				int[] dcRefine = new int[count + 4]; for (int c = 0; c < count; ++c) { dcRefine[c] = c; } dcRefine[count + 2] = 1; scans.Add(dcRefine);
				for (int c = 1; c < count; ++c) { scans.Add(new int[] {c, 1, 63, 1, 0}); }
				scans.Add(new int[] {0, 1, 63, 1, 0});
			}
		}
		bool redefined = false;
		foreach (int[] scan in scans) {
			int n = scan.Length - 4;
			if (a_shape.RedefineTables && !redefined && scan[0] != 0) {
				file.AddRange(Segment(0xdb, Quantization(0, chromaTable, false)));
				file.AddRange(Segment(0xc4, Huffman(0)));
				redefined = true;
			}
			List<byte> head = new List<byte> {(byte)n};
			for (int i = 0; i < n; ++i) { head.Add((byte)(scan[i] + 1)); head.Add(0x00); }
			head.Add((byte)scan[n]); head.Add((byte)scan[n + 1]); head.Add((byte)((scan[n + 2] << 4) | scan[n + 3]));
			file.AddRange(Segment(0xda, head));
			file.AddRange(Entropy(planes, scan, n, mcusAcross, mcusDown, a_shape));
		}
		file.Add(0xff); file.Add(0xd9);
		return file.ToArray();
	}

	static List<byte> Entropy(Plane[] a_planes, int[] a_scan, int a_count, int a_mcusAcross, int a_mcusDown, JpegShape a_shape) {
		Bits bits = new Bits();
		int ss = a_scan[a_count], se = a_scan[a_count + 1], ah = a_scan[a_count + 2], al = a_scan[a_count + 3];
		int[] prediction = new int[3];
		int eobRun = 0;
		Action flushRun = () => {
			if (eobRun == 0) { return; }
			int size = BitLength(eobRun) - 1;
			Ac(bits, size << 4);
			bits.Put(eobRun - (1 << size), size);
			eobRun = 0;
		};
		List<int[]> order = new List<int[]>();   // plane, block x, block y
		if (a_count == 1) {
			Plane p = a_planes[a_scan[0]];
			int across = (p.Width + 7) / 8, down = (p.Height + 7) / 8;
			for (int y = 0; y < down; ++y) { for (int x = 0; x < across; ++x) { order.Add(new int[] {a_scan[0], x, y, y * across + x}); } }
		} else {
			for (int my = 0; my < a_mcusDown; ++my) {
				for (int mx = 0; mx < a_mcusAcross; ++mx) {
					for (int i = 0; i < a_count; ++i) {
						Plane p = a_planes[a_scan[i]];
						for (int y = 0; y < p.V; ++y) { for (int x = 0; x < p.H; ++x) { order.Add(new int[] {a_scan[i], mx * p.H + x, my * p.V + y, my * a_mcusAcross + mx}); } }
					}
				}
			}
		}
		int lastMcu = -1, mcus = 0, restarts = 0;
		foreach (int[] at in order) {
			if (at[3] != lastMcu) {
				if (a_shape.Restart != 0 && mcus != 0 && mcus % a_shape.Restart == 0) {
					flushRun();
					bits.Marker(0xd0 + (restarts++ & 7));
					prediction = new int[3];
				}
				lastMcu = at[3];
				++mcus;
			}
			Plane plane = a_planes[at[0]];
			int[] block = plane.Blocks[at[2] * plane.BlocksAcross + at[1]];
			bool sequential = !a_shape.Progressive;
			if (ss == 0) {
				if (ah == 0) {
					int value = block[0] >> al;
					Dc(bits, value - prediction[at[0]]);
					prediction[at[0]] = value;
				} else {
					bits.Put((block[0] >> al) & 1, 1);
				}
				if (!sequential) { continue; }
			}
			if (ah == 0) {
				int run = 0;
				for (int k = sequential ? 1 : ss; k <= se; ++k) {
					int coefficient = block[Zigzag[k]];
					int magnitude = Math.Abs(coefficient) >> al;
					if (magnitude == 0) { ++run; continue; }
					flushRun();
					while (run > 15) { Ac(bits, 0xf0); run -= 16; }
					AcValue(bits, run, coefficient < 0 ? -magnitude : magnitude);
					run = 0;
				}
				if (run > 0 && !a_shape.Progressive) { Ac(bits, 0x00); }
				else if (run > 0) {
					++eobRun;
					if (eobRun == 0x7fff || !a_shape.EobRuns) { flushRun(); }
				}
				continue;
			}
			// a refinement scan, one EOB a block: the last coefficient this scan makes nonzero bounds the ZRLs
			int[] absolute = new int[64];
			int last = -1;
			for (int k = ss; k <= se; ++k) {
				absolute[k] = Math.Abs(block[Zigzag[k]]) >> al;
				if (absolute[k] == 1) { last = k; }
			}
			int zeros = 0;
			List<int> corrections = new List<int>();
			for (int k = ss; k <= se; ++k) {
				int magnitude = absolute[k];
				if (magnitude == 0) { ++zeros; continue; }
				while (zeros > 15 && k <= last) {
					Ac(bits, 0xf0);
					zeros -= 16;
					foreach (int c in corrections) { bits.Put(c, 1); }
					corrections.Clear();
				}
				if (magnitude > 1) { corrections.Add(magnitude & 1); continue; }
				Ac(bits, (zeros << 4) | 1);
				bits.Put(block[Zigzag[k]] < 0 ? 0 : 1, 1);
				foreach (int c in corrections) { bits.Put(c, 1); }
				corrections.Clear();
				zeros = 0;
			}
			if (zeros > 0 || corrections.Count > 0) {
				Ac(bits, 0x00);
				foreach (int c in corrections) { bits.Put(c, 1); }
			}
		}
		flushRun();
		bits.Flush();
		return bits.Out;
	}
}

// A GIF writer for the shapes Windows' encoders do not write: interlacing, a frame smaller than its screen at an offset, a local
// colour table, any minimum code size, a table left full with no clear code, 87a, and frames after the first. Windows'
// decoder reads each file for its reference pixels.
public class GifShape {
	public int ScreenWidth, ScreenHeight, Left, Top;
	public bool Interlaced, LocalTable, ClearWhenFull = true, Old;
	public int Transparent = -1;
	public int Minimum;
	public bool SecondFrame;
}

public static class GifWriter {
	class Bits {
		public List<byte> Out = new List<byte>();
		int buffer, used;
		public void Put(int a_code, int a_size) {
			buffer |= a_code << used;
			used += a_size;
			while (used >= 8) { Out.Add((byte)buffer); buffer >>= 8; used -= 8; }
		}
		public void Flush() { if (used > 0) { Out.Add((byte)buffer); buffer = 0; used = 0; } }
	}

	static List<byte> Lzw(byte[] a_indexes, int a_minimum, bool a_clearWhenFull) {
		int clear = 1 << a_minimum, end = clear + 1;
		Bits bits = new Bits();
		Dictionary<int, int> table = new Dictionary<int, int>();
		int next = clear + 2, size = a_minimum + 1;
		bits.Put(clear, size);
		int prefix = a_indexes[0];
		for (int i = 1; i < a_indexes.Length; ++i) {
			int k = a_indexes[i];
			int key = (prefix << 8) | k;
			int found;
			if (table.TryGetValue(key, out found)) { prefix = found; continue; }
			if (next > (1 << size) && size < 12) { ++size; }
			bits.Put(prefix, size);
			if (next < 4096) {
				table[key] = next++;
			} else if (a_clearWhenFull) {
				if (next > (1 << size) && size < 12) { ++size; }
				bits.Put(clear, size);
				table.Clear();
				next = clear + 2;
				size = a_minimum + 1;
			}
			prefix = k;
		}
		if (next > (1 << size) && size < 12) { ++size; }
		bits.Put(prefix, size);
		if (next > (1 << size) && size < 12) { ++size; }
		bits.Put(end, size);
		bits.Flush();
		return bits.Out;
	}

	static void SubBlocks(List<byte> a_file, List<byte> a_data) {
		for (int at = 0; at < a_data.Count; at += 255) {
			int n = Math.Min(255, a_data.Count - at);
			a_file.Add((byte)n);
			a_file.AddRange(a_data.GetRange(at, n));
		}
		a_file.Add(0);
	}

	// a_indexes (a_width x a_height, row by row) as the first frame of a GIF over a_palette (R, G, B triples)
	public static byte[] Write(byte[] a_indexes, int a_width, int a_height, byte[] a_palette, GifShape a_shape) {
		int entries = a_palette.Length / 3, bits = 1;
		while ((1 << bits) < entries) { ++bits; }
		byte[] table = new byte[3 << bits];
		Array.Copy(a_palette, table, a_palette.Length);
		List<byte> file = new List<byte>();
		file.AddRange(Encoding.ASCII.GetBytes(a_shape.Old ? "GIF87a" : "GIF89a"));
		int screenWidth = a_shape.ScreenWidth != 0 ? a_shape.ScreenWidth : a_width, screenHeight = a_shape.ScreenHeight != 0 ? a_shape.ScreenHeight : a_height;
		file.Add((byte)screenWidth); file.Add((byte)(screenWidth >> 8)); file.Add((byte)screenHeight); file.Add((byte)(screenHeight >> 8));
		if (a_shape.LocalTable) {
			// a global table of other colours, which the frame's own table replaces
			file.Add(0x80 | 0x70 | 0); file.Add(0); file.Add(0);
			file.AddRange(new byte[] {9, 9, 9, 250, 250, 250});
		} else {
			file.Add((byte)(0x80 | 0x70 | (bits - 1))); file.Add(0); file.Add(0);
			file.AddRange(table);
		}
		if (a_shape.Transparent >= 0) { file.AddRange(new byte[] {0x21, 0xf9, 4, 1, 0, 0, (byte)a_shape.Transparent, 0}); }
		// a comment, which a decoder skips
		file.AddRange(new byte[] {0x21, 0xfe, 3, (byte)'j', (byte)'a', (byte)'w', 0});
		file.Add(0x2c);
		file.Add((byte)a_shape.Left); file.Add((byte)(a_shape.Left >> 8)); file.Add((byte)a_shape.Top); file.Add((byte)(a_shape.Top >> 8));
		file.Add((byte)a_width); file.Add((byte)(a_width >> 8)); file.Add((byte)a_height); file.Add((byte)(a_height >> 8));
		file.Add((byte)((a_shape.LocalTable ? 0x80 | (bits - 1) : 0) | (a_shape.Interlaced ? 0x40 : 0)));
		if (a_shape.LocalTable) { file.AddRange(table); }
		byte[] ordered = a_indexes;
		if (a_shape.Interlaced) {
			List<byte> rows = new List<byte>();
			int[][] passes = {new int[] {0, 8}, new int[] {4, 8}, new int[] {2, 4}, new int[] {1, 2}};
			foreach (int[] pass in passes) {
				for (int y = pass[0]; y < a_height; y += pass[1]) { for (int x = 0; x < a_width; ++x) { rows.Add(a_indexes[y * a_width + x]); } }
			}
			ordered = rows.ToArray();
		}
		int minimum = a_shape.Minimum != 0 ? a_shape.Minimum : Math.Max(2, bits);
		file.Add((byte)minimum);
		SubBlocks(file, Lzw(ordered, minimum, a_shape.ClearWhenFull));
		if (a_shape.SecondFrame) {
			// a second frame of other pixels, which only an animation would show
			file.AddRange(new byte[] {0x2c, 0, 0, 0, 0, (byte)a_width, (byte)(a_width >> 8), (byte)a_height, (byte)(a_height >> 8), 0, (byte)minimum});
			byte[] other = new byte[a_width * a_height];
			for (int i = 0; i < other.Length; ++i) { other[i] = (byte)((i * 7) % entries); }
			SubBlocks(file, Lzw(other, minimum, true));
		}
		file.Add(0x3b);
		return file.ToArray();
	}
}
