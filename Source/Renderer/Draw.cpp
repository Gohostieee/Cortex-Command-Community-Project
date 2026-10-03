#include "Draw.h"
#include "GLResourceMan.h"
#include "MultiplayerWorld.h"

namespace RTE {
	void DrawTexture(BITMAP* bitmap, int posX, int posY, RLColor tint) {
		if (MultiplayerWorld::CanvasSprite(bitmap, {0, 0, float(bitmap->w), float(bitmap->h)}, {float(posX), float(posY), float(bitmap->w), float(bitmap->h)}, {}, 0, tint)) return;
		DrawTexture(g_GLResourceMan.GetStaticTextureFromBitmap(bitmap), posX, posY, tint);
	}

	void DrawTextureV(BITMAP* bitmap, Vector2 pos, RLColor tint) {
		if (MultiplayerWorld::CanvasSprite(bitmap, {0, 0, float(bitmap->w), float(bitmap->h)}, {pos.x, pos.y, float(bitmap->w), float(bitmap->h)}, {}, 0, tint)) return;
		DrawTextureV(g_GLResourceMan.GetStaticTextureFromBitmap(bitmap), pos, tint);
	}

	void DrawTextureEx(BITMAP* bitmap, Vector2 pos, float rotation, float scale, RLColor tint) {
		if (MultiplayerWorld::CanvasSprite(bitmap, {0, 0, float(bitmap->w), float(bitmap->h)}, {pos.x, pos.y, bitmap->w * scale, bitmap->h * scale}, {}, rotation, tint)) return;
		DrawTextureEx(g_GLResourceMan.GetStaticTextureFromBitmap(bitmap), pos, rotation, scale, tint);
	}
	void DrawTextureRec(BITMAP* bitmap, Rectangle source, Vector2 pos, RLColor tint) {
		if (MultiplayerWorld::CanvasSprite(bitmap, source, {pos.x, pos.y, std::abs(source.width), std::abs(source.height)}, {}, 0, tint)) return;
		DrawTextureRec(g_GLResourceMan.GetStaticTextureFromBitmap(bitmap), source, pos, tint);
	}
	void DrawTexturePro(BITMAP* bitmap, Rectangle source, Rectangle dest, Vector2 origin, float rotation, RLColor tint) {
		if (MultiplayerWorld::CanvasSprite(bitmap, source, dest, origin, rotation, tint)) return;
		DrawTexturePro(g_GLResourceMan.GetStaticTextureFromBitmap(bitmap), source, dest, origin, rotation, tint);
	}
}
