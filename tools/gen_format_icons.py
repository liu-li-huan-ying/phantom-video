"""Generate video format icons for VPlayer playlist panel."""
import os
from PIL import Image, ImageDraw, ImageFont

OUTPUT_DIR = r"F:\vedioplayer\assets\icons\formats"
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Format -> (label, bg_color, text_color)
FORMATS = {
    "mp4":  ("MP4",  (41, 128, 185),   (255, 255, 255)),
    "m4v":  ("M4V",  (41, 128, 185),   (255, 255, 255)),
    "mkv":  ("MKV",  (39, 174, 96),    (255, 255, 255)),
    "avi":  ("AVI",  (230, 126, 34),   (255, 255, 255)),
    "wmv":  ("WMV",  (142, 68, 173),   (255, 255, 255)),
    "asf":  ("ASF",  (142, 68, 173),   (255, 255, 255)),
    "mov":  ("MOV",  (0, 172, 193),    (255, 255, 255)),
    "flv":  ("FLV",  (241, 196, 15),   (30, 30, 30)),
    "f4v":  ("F4V",  (241, 196, 15),   (30, 30, 30)),
    "rm":   ("RM",   (231, 76, 60),    (255, 255, 255)),
    "rmvb": ("RMVB", (231, 76, 60),    (255, 255, 255)),
    "3gp":  ("3GP",  (236, 100, 175),  (255, 255, 255)),
    "mpg":  ("MPG",  (139, 90, 43),    (255, 255, 255)),
    "mpeg": ("MPEG", (139, 90, 43),    (255, 255, 255)),
    "vob":  ("VOB",  (127, 140, 141),  (255, 255, 255)),
    "webm": ("WEBM", (22, 160, 133),   (255, 255, 255)),
    "ogv":  ("OGV",  (22, 160, 133),   (255, 255, 255)),
    "ts":   ("TS",   (52, 152, 219),   (255, 255, 255)),
    "mts":  ("MTS",  (52, 152, 219),   (255, 255, 255)),
    "m2ts": ("M2TS", (52, 152, 219),   (255, 255, 255)),
    "gif":  ("GIF",  (155, 89, 182),   (255, 255, 255)),
    "swf":  ("SWF",  (192, 57, 43),    (255, 255, 255)),
}

SIZE = 48
CORNER_R = 8

def make_icon(label, bg, fg):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    # Rounded rectangle background
    draw.rounded_rectangle([(0, 0), (SIZE-1, SIZE-1)], radius=CORNER_R, fill=bg)
    # Try to find a good font
    font = None
    font_size = 14 if len(label) <= 3 else 11
    for fp in ["C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\arial.ttf"]:
        if os.path.exists(fp):
            try:
                font = ImageFont.truetype(fp, font_size)
                break
            except:
                pass
    if font is None:
        font = ImageFont.load_default()
    # Center text
    bbox = draw.textbbox((0, 0), label, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx = (SIZE - tw) // 2
    ty = (SIZE - th) // 2
    draw.text((tx, ty), label, fill=fg, font=font)
    return img

for ext, (label, bg, fg) in FORMATS.items():
    img = make_icon(label, bg, fg)
    path = os.path.join(OUTPUT_DIR, f"{ext}.png")
    img.save(path, "PNG")
    print(f"  {ext}.png")

print(f"Done: {len(FORMATS)} icons")
