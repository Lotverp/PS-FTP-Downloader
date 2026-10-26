#!/usr/bin/env python3
"""
PS4 Assets Generator (icon0.png 512x512 and pic1.png 1920x1080)
Uses Pillow to create high-quality PlayStation 4 dashboard visuals.
"""

import sys
import os
from PIL import Image, ImageDraw, ImageFilter

def generate_assets(src_icon_path, out_dir="sce_sys"):
    os.makedirs(out_dir, exist_ok=True)
    icon0_path = os.path.join(out_dir, "icon0.png")
    pic1_path = os.path.join(out_dir, "pic1.png")
    
    # 1. Generate icon0.png (512x512)
    if os.path.exists(src_icon_path):
        src_img = Image.open(src_icon_path).convert("RGBA")
        icon0 = src_img.resize((512, 512), Image.Resampling.LANCZOS)
        icon0.save(icon0_path, "PNG")
        print(f"[OK] icon0.png generated at {icon0_path} (512x512)")
    else:
        # Fallback icon
        icon0 = Image.new("RGBA", (512, 512), (20, 24, 38, 255))
        draw = ImageDraw.Draw(icon0)
        draw.rounded_rectangle([32, 32, 480, 480], radius=40, fill=(30, 41, 59, 255), outline=(56, 189, 248, 255), width=6)
        icon0.save(icon0_path, "PNG")
        print(f"[OK] Fallback icon0.png generated at {icon0_path}")

    # 2. Generate pic1.png (1920x1080 banner)
    bg = Image.new("RGBA", (1920, 1080), (10, 14, 26, 255))
    draw = ImageDraw.Draw(bg)
    
    # Subtle deep-blue/purple PlayStation styled gradient waves
    for y in range(1080):
        ratio = y / 1080.0
        r = int(12 + ratio * 15)
        g = int(18 + ratio * 28)
        b = int(35 + ratio * 55)
        draw.line([(0, y), (1920, y)], fill=(r, g, b, 255))
        
    # Place subtle logo in right area
    if os.path.exists(src_icon_path):
        logo = Image.open(src_icon_path).convert("RGBA")
        logo = logo.resize((600, 600), Image.Resampling.LANCZOS)
        
        # Soft glow / shadow
        bg.paste(logo, (1200, 240), logo)

    # Gradient overlay for text legibility
    overlay = Image.new("RGBA", (1920, 1080), (0, 0, 0, 0))
    d_ov = ImageDraw.Draw(overlay)
    for x in range(1200):
        alpha = int((1.0 - (x / 1200.0)) * 140)
        d_ov.line([(x, 0), (x, 1080)], fill=(0, 0, 0, alpha))
        
    final_pic1 = Image.alpha_composite(bg, overlay)
    final_pic1.convert("RGB").save(pic1_path, "PNG")
    print(f"[OK] pic1.png generated at {pic1_path} (1920x1080)")

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
    default_icon = os.path.join(repo_root, "gtk-app", "ps-ftp-downloader.png")
    default_out = os.path.abspath(os.path.join(script_dir, "..", "sce_sys"))

    src = sys.argv[1] if len(sys.argv) > 1 else default_icon
    out = sys.argv[2] if len(sys.argv) > 2 else default_out
    generate_assets(src, out)
