# Draws every screen of the app to tests/preview/out/*.png on a PC (needs jansson + Python Pillow).
# The console font is extracted from libctru: see README in this folder.
preview: preview.c ../../source/main.c
	mkdir -p out
	cc -O1 -w -I. -I../../source -o preview preview.c ../../source/nav.c ../../source/render.c ../../source/nmea.c -lm -ljansson
	./preview
	python3 -c "from PIL import Image; import glob; [Image.open(f).resize((640 if '/top' not in f else 800, 480), Image.NEAREST).save(f[:-4]+'.png') for f in glob.glob('out/*.ppm')]"
