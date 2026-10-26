#!/usr/bin/env python3
"""
PS4 GP4 Project Generator for orbis-pub-cmd
Creates the packaging XML specification used to produce official Fake PKGs.
"""

import sys
import os
import datetime

def generate_gp4(base_dir=".", output_path="app.gp4", title_id="PSFT00001"):
    content_id = f"IV0001-{title_id}_00-PS4DOWNLOADER000"
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    # Collect files
    files_to_pack = [
        ("eboot.bin", os.path.join(base_dir, "build", "eboot.bin")),
        ("sce_sys/param.sfo", os.path.join(base_dir, "sce_sys", "param.sfo")),
        ("sce_sys/icon0.png", os.path.join(base_dir, "sce_sys", "icon0.png")),
        ("sce_sys/pic1.png", os.path.join(base_dir, "sce_sys", "pic1.png")),
    ]
    
    xml = [
        '<?xml version="1.0" encoding="utf-8" standalone="yes"?>',
        '<psproject fmt="gp4" version="1000">',
        '  <volume>',
        '    <volume_type>pkg_ps4_app</volume_type>',
        '    <volume_id>PS4DOWNLOADER</volume_id>',
        f'    <volume_ts>{ts}</volume_ts>',
        f'    <package content_id="{content_id}" passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full" />',
        '  </volume>',
        '  <files>',
    ]
    
    for targ, orig in files_to_pack:
        xml.append(f'    <file targ_path="{targ}" orig_path="{orig}" />')
        
    xml.extend([
        '  </files>',
        '  <rootdir>',
        '    <dir targ_name="sce_sys" />',
        '  </rootdir>',
        '</psproject>'
    ])
    
    content = "\n".join(xml) + "\n"
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"[OK] app.gp4 generated at {output_path}")

if __name__ == "__main__":
    bdir = sys.argv[1] if len(sys.argv) > 1 else "."
    out = sys.argv[2] if len(sys.argv) > 2 else "app.gp4"
    generate_gp4(bdir, out)
