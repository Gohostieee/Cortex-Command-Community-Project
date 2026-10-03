"""Generate indexed terrain for the native mining fixture, without random debris."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
terrain = Image.new("P", (960, 540), 0)
terrain.putpalette(Image.open(root / "Data/Base.rte/Scenes/Terrains/KetanotHills.png").getpalette())
draw = ImageDraw.Draw(terrain)
draw.rectangle((0, 140, 959, 539), fill=10)
draw.rectangle((0, 534, 959, 539), fill=3)  # solid world floor, keeping miners in the scene
for box in ((80, 155, 103, 171), (420, 320, 443, 339), (880, 475, 903, 494)):
    draw.rectangle(box, fill=2)
for pixel in ((20, 200), (939, 230), (660, 450)):
    draw.point(pixel, fill=2)
terrain.save(root / "Tests/GoldMining.rte/Terrain.png")
