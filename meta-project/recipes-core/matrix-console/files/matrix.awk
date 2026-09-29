# Recheck dimensions about once a second, including after HDMI mode changes.
function dimensions(    command, size, fields, rows, cols) {
    command = "stty size < /dev/tty"
    rows = 24; cols = 80
    if ((command | getline size) > 0 && split(size, fields, " ") == 2 &&
        fields[1] ~ /^[0-9]+$/ && fields[2] ~ /^[0-9]+$/ &&
        fields[1] > 0 && fields[2] > 0) {
        rows = fields[1] + 0; cols = fields[2] + 0
    }
    close(command)
    if (rows != h || cols != w) {
        h = rows; w = cols
        delete rain; delete len; delete head
        ax = int((w - aw) / 2); ay = int((h - ah) / 2)
        printf "\033[2J"
    }
}

BEGIN {
	s = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789$+-*/=%#&@<>:;|^~"
	n = length(s)
	for (i = 1; i <= n; i++) ch[i] = substr(s, i, 1)
	# Store the underlying rain separately: scrolling painted artwork would
	# leave copies of the guy trailing down the screen.
	while ((getline line < artfile) > 0) {
		art[++ah] = line
		if (length(line) > aw) aw = length(line)
	}
	close(artfile)
	srand()
	for (;;) {
		if (frames++ % 16 == 0) dimensions()
		for (r = h; r > 1; r--)
			for (c = 1; c <= w; c++) rain[r, c] = rain[r - 1, c]
		for (c = 1; c <= w; c++) head[c] = 0
		for (c = 1; c <= w; c++) {
			if (len[c] > 0) {
				k = ch[int(rand() * n) + 1]
				rain[1, c] = k
				len[c]--
			} else if (rand() < 0.02) {
				k = ch[int(rand() * n) + 1]
				# the leading edge of a streak is drawn bright white
				rain[1, c] = k; head[c] = 1
				len[c] = 4 + int(rand() * 20)
			} else {
				rain[1, c] = " "
			}
		}
		frame = ""
		for (r = 1; r <= h; r++) {
			frame = frame sprintf("\033[%d;1H", r)
			color = ""
			for (c = 1; c <= w; c++) {
				# Do not write the bottom-right cell: it can scroll the console.
				if (r == h && c == w) break
				glyph = ""
				if (r > ay && r <= ay + ah && c > ax && c <= ax + aw)
					glyph = substr(art[r - ay], c - ax, 1)
				if (glyph != "" && glyph != " ") shade = "\033[1;37m"
				else {
					glyph = rain[r, c]
					if (glyph == "") glyph = " "
					shade = r == 1 && head[c] ? "\033[1;37m" : "\033[0;32m"
				}
				if (shade != color) { frame = frame shade; color = shade }
				frame = frame glyph
			}
		}
		printf "%s\033[0m", frame
		fflush()
		system("sleep 0.06")
	}
}
