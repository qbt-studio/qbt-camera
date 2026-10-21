from PIL import Image, ImageDraw, ImageFilter, ImageFont
import os

W, H = 1280, 640
banner = Image.new("RGBA", (W, H), (11, 15, 25, 255))

# Draw gradient background
bg_draw = ImageDraw.Draw(banner)
for y in range(H):
    r = int(11 + (22 - 11) * (y / H))
    g = int(15 + (30 - 15) * (y / H))
    b = int(25 + (50 - 25) * (y / H))
    bg_draw.line([(0, y), (W, y)], fill=(r, g, b, 255))

# Subtle blue/purple glow in the center-left and right
glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
glow_draw = ImageDraw.Draw(glow)
glow_draw.ellipse([200, 100, 900, 700], fill=(20, 80, 180, 45))
glow_draw.ellipse([800, 50, 1300, 600], fill=(130, 40, 200, 35))
glow = glow.filter(ImageFilter.GaussianBlur(80))
banner = Image.alpha_composite(banner, glow)

# Load screenshots
win_path = r"C:\FILES\QBTCameraStudio\docs\screenshots\windows-portrait.png"
iphone_path = r"C:\FILES\QBTCameraStudio\docs\screenshots\iphone-background.png"
icon_path = r"C:\FILES\QBTCameraStudio\docs\screenshots\icon.png"

# Windows screenshot scaled
win_img = Image.open(win_path).convert("RGBA")
# Target height ~ 430
win_w = int(win_img.width * (430 / win_img.height))
win_resized = win_img.resize((win_w, 430), Image.Resampling.LANCZOS)

# Create a shadow for Windows window
shadow_w, shadow_h = win_w + 40, 470
shadow = Image.new("RGBA", (shadow_w, shadow_h), (0, 0, 0, 0))
sdraw = ImageDraw.Draw(shadow)
sdraw.rounded_rectangle([20, 20, shadow_w - 20, shadow_h - 20], radius=16, fill=(0, 0, 0, 140))
shadow = shadow.filter(ImageFilter.GaussianBlur(15))

# Paste Windows preview on left
win_x, win_y = 60, 145
banner.paste(shadow, (win_x - 20, win_y - 20), shadow)
banner.paste(win_resized, (win_x, win_y), win_resized)

# iPhone screenshot scaled
iph_img = Image.open(iphone_path).convert("RGBA")
iph_h = 490
iph_w = int(iph_img.width * (iph_h / iph_img.height))
iph_resized = iph_img.resize((iph_w, iph_h), Image.Resampling.LANCZOS)

# iPhone border / frame (rounded rectangle)
mask = Image.new("L", (iph_w, iph_h), 0)
mdraw = ImageDraw.Draw(mask)
mdraw.rounded_rectangle([0, 0, iph_w, iph_h], radius=28, fill=255)

iph_shadow = Image.new("RGBA", (iph_w + 50, iph_h + 50), (0, 0, 0, 0))
isdraw = ImageDraw.Draw(iph_shadow)
isdraw.rounded_rectangle([25, 25, iph_w + 25, iph_h + 25], radius=28, fill=(0, 0, 0, 180))
iph_shadow = iph_shadow.filter(ImageFilter.GaussianBlur(20))

iph_x, iph_y = 960, 105
banner.paste(iph_shadow, (iph_x - 25, iph_y - 25), iph_shadow)
banner.paste(iph_resized, (iph_x, iph_y), mask)

# Stroke around iPhone
stroke_img = Image.new("RGBA", (iph_w, iph_h), (0, 0, 0, 0))
s_draw = ImageDraw.Draw(stroke_img)
s_draw.rounded_rectangle([0, 0, iph_w, iph_h], radius=28, outline=(255, 255, 255, 80), width=3)
banner.paste(stroke_img, (iph_x, iph_y), stroke_img)

# App icon and Title text in top left
icon_img = Image.open(icon_path).convert("RGBA").resize((68, 68), Image.Resampling.LANCZOS)
banner.paste(icon_img, (60, 42), icon_img)

draw = ImageDraw.Draw(banner)
# Try system font or default
try:
    font_title = ImageFont.truetype("arialbd.ttf", 36)
    font_sub = ImageFont.truetype("arial.ttf", 20)
    font_badge = ImageFont.truetype("arialbd.ttf", 14)
except Exception:
    font_title = font_sub = font_badge = None

draw.text((145, 42), "QBT Camera", font=font_title, fill=(255, 255, 255, 255))
draw.text((148, 86), "Pro iPhone Webcam for Windows  •  LiDAR Depth Bokeh  •  No Watermark", font=font_sub, fill=(160, 180, 215, 255))

# Save social preview banner
output_path = r"C:\FILES\QBTCameraStudio\docs\screenshots\social-preview.png"
banner.convert("RGB").save(output_path, quality=95)
print(f"Generated {output_path} (1280x640)")
