#!/usr/bin/env python3
# make_blood_colours.py SRC OUT : green and yellow copies of the game's floor blood (dag_floorblood1..6).
# SRC holds the originals extracted from the game (os_models.bif: .mdl, os_textures.bif: .tga). The model
# copies only rename the model / node / texture (same length, so the binary layout stays); the textures are
# recoloured from the red channel, keeping the alpha.
import sys, numpy as np
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
tint = {'gr': (0.42, 0.78, 0.18), 'ye': (0.95, 0.80, 0.20)}
for i in range(1, 7):
    mdl = open(f'{src}/dag_floorblood{i}.mdl', 'rb').read()
    a = np.asarray(Image.open(f'{src}/dag_floorblood{i}.tga').convert('RGBA')).astype(np.float32)
    for tag, (r, g, b) in tint.items():
        name = f'dag_floorbld{tag}{i}'
        open(f'{out}/{name}.mdl', 'wb').write(mdl.replace(f'dag_floorblood{i}'.encode(), name.encode()))
        c = np.stack([a[..., 0] * r, a[..., 0] * g, a[..., 0] * b, a[..., 3]], -1)
        Image.fromarray(np.clip(c, 0, 255).astype(np.uint8), 'RGBA').save(f'{out}/{name}.tga')
