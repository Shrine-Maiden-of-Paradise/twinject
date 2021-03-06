#include "algo/th_vo_algo.h"

#include <numeric>

#include <imgui.h>

#include "config/th_config.h"
#include "control/movement.h"
#include "control/th_player.h"
#include "control/th11_player.h"
#include "control/th15_player.h"
#include "control/th10_player.h"
#include "gfx/imgui_mixins.h"
#include "hook/th_di8_hook.h"
#include "util/cdraw.h"
#include "util/color.h"

void th_vo_algo::onBegin()
{
	calibInit();
}

void th_vo_algo::onTick()
{
	/* IMGUI Integration */
	using namespace ImGui;
	Begin("th_vo_algo");
	Text("Constrained Velocity Obstacle Algorithm");
	if (CollapsingHeader("Info", ImGuiTreeNodeFlags_DefaultOpen))
	{
		Text("calib: %s", isCalibrated ? "true" : "false");
		SameLine(); ShowHelpMarker("Algorithm player speed calibration");

		Text("calib vel: norm %.2f, foc %.2f", playerVel, playerFocVel);
		SameLine(); ShowHelpMarker("Calibrated velocities in normal and focused mode");

		Text("col test: %s", hitCircle ? "hit circle" : "hit box");
		SameLine(); ShowHelpMarker("Collision test used");
	}

	auto di8 = th_di8_hook::inst();

	if (!player->enabled) {
		di8->resetVkState(DIK_LEFT);
		di8->resetVkState(DIK_RIGHT);
		di8->resetVkState(DIK_UP);
		di8->resetVkState(DIK_DOWN);
		di8->resetVkState(DIK_Z);
		di8->resetVkState(DIK_LSHIFT);
		di8->resetVkState(DIK_LCONTROL);
		End();
		return;
	}

	if (!isCalibrated)
	{
		isCalibrated = calibTick();
		if (isCalibrated) {
			SPDLOG_INFO("calibrated plyr vel: {} {}", playerVel, playerFocVel);
		}
		End();
		return;
	}

	auto plyr = player->getPlayerEntity();

	/*
	 * Ticks until collision whilst moving in this direction
	 * Uses same direction numbering schema
	 */
	float collisionTicks[control::Movement::MaxValue];
	std::fill_n(collisionTicks, control::Movement::MaxValue, FLT_MAX);

	bool bounded = true;

	std::shared_ptr<entity> pseudoPlayers[control::Movement::MaxValue];
	for (int dir = 0; dir < control::Movement::MaxValue; ++dir)
	{
		vec2 pvel = this->getPlayerMovement(dir);
		pseudoPlayers[dir] = plyr.obj->withVelocity(pvel);
	}

	// Bullet collision frame calculations
	for (auto b = player->bullets.begin(); b != player->bullets.end(); ++b)
	{
		for (int dir = 0; dir < control::Movement::MaxValue; ++dir)
		{
			const auto pseudoPlayer = pseudoPlayers[dir];
			float colTick = pseudoPlayer->willCollideWith(*b->obj);

			if (colTick >= 0) {
				collisionTicks[dir] = std::min(colTick, collisionTicks[dir]);
				bounded = false;
			}
		}
	}

	for (auto b = player->enemies.begin(); b != player->enemies.end(); ++b)
	{
		for (int dir = 0; dir < control::Movement::MaxValue; ++dir)
		{
			const auto pseudoPlayer = pseudoPlayers[dir];
			float colTick = pseudoPlayer->willCollideWith(*b->obj);

			if (colTick >= 0) {
				collisionTicks[dir] = std::min(colTick, collisionTicks[dir]);
				bounded = false;
			}
		}
	}

	for (laser l : player->lasers)
	{
		for (int dir = 0; dir < control::Movement::MaxValue; ++dir)
		{
			const auto pseudoPlayer = pseudoPlayers[dir];
			float colTick = pseudoPlayer->willCollideWith(*l.obj);

			if (colTick >= 0) {
				collisionTicks[dir] = std::min(colTick, collisionTicks[dir]);
				bounded = false;
			}
		}
	}

	/*
	 * Powerup collision frame calculations
	 * Note: Powerups do not move linearly so using a linear model might be poor.
	 */

	 // Ticks until collision with target whilst moving in this direction
	float targetTicks[control::Movement::MaxValue];
	std::fill_n(targetTicks, control::Movement::MaxValue, FLT_MAX);

	for (const auto& powerup : player->powerups)
	{
		// Filter out unwanted powerups
		if (powerup.meta == 0 && powerup.obj->com().y > 200) {
			for (int dir = 0; dir < control::Movement::MaxValue; ++dir)
			{
				const auto pseudoPlayer = pseudoPlayers[dir];

				/*
				 * Powerups tend to be attracted towards the player, so we can be
				 * very lax with the collision predictor and use the AABB model
				 * all the time
				 */
				 /*float colTick = vec2::willCollideAABB(
						 plyr.position - plyr.size / 2, powerup.position - powerup.size / 2, plyr.size, powerup.size, pvel, powerup.velocity);
				 */
				float colTick = pseudoPlayer->willCollideWith(*powerup.obj);
				if (colTick >= 0) {
					targetTicks[dir] = std::min(colTick, targetTicks[dir]);
				}
			}
		}
	}

	// We should probably prioritize larger enemies over smaller ones, 
	// and prioritize powerup gathering over enemies
	for (const auto& enemy : player->enemies)
	{
		const vec2 enemyCom = enemy.obj->com();
		const vec2 playerCom = plyr.obj->com();
		if (enemyCom.y < playerCom.y) {
			for (int dir : {control::Movement::Left, control::Movement::Right})
			{
				const vec2 pvel = this->getPlayerMovement(dir);

				// Calculate x-distance to y-aligned axis of the enemy
				float xDist = enemyCom.x - playerCom.x;
				float colTick = xDist / pvel.x;
				// Filter out impossible values
				if (colTick >= 0 && colTick <= 6000)
				{
					targetTicks[dir] = std::min(colTick, targetTicks[dir]);
				}
			}
		}
	}

	aabb gameBounds{ vec2(), vec2(), vec2(th_param.GAME_WIDTH, th_param.GAME_HEIGHT) };
	// Wall collision frame calculations
	for (int dir = 1; dir < control::Movement::MaxValue; ++dir)
	{
		const auto pseudoPlayer = pseudoPlayers[dir];


		float t = pseudoPlayer->willExit(gameBounds);
		/*float t = vec2::willExitAABB(
			vec2(0, 0), plyr.position - plyr.size / 2, vec2(th_param.GAME_WIDTH, th_param.GAME_HEIGHT),
			plyr.size, vec2(), pvel);*/
		if (t >= 0) {
			if (t < collisionTicks[dir])
				collisionTicks[dir] = t;
		}
	}
	// Look for best viable target, aka targeting will not result in collision
	int tarIdx = -1;
	float min_collision_tick = *std::min_element(collisionTicks + control::Movement::Up, collisionTicks + control::Movement::MaxValue);
	float max_collision_tick = *std::max_element(collisionTicks, collisionTicks + control::Movement::MaxValue);
	if (min_collision_tick > 1.f && max_collision_tick > 100.f)
	{
		for (int dir = 0; dir < control::Movement::MaxValue; ++dir)
		{
			if (targetTicks[dir] < collisionTicks[dir]
				&& (tarIdx == -1 || targetTicks[dir] < targetTicks[tarIdx]))
				tarIdx = dir;
		}
	}

	Text("target found: %s", (tarIdx == -1) ? "false" : "true");

	bool powerupTarget = true;
	// check if we could find a targetable powerup
	if (tarIdx == -1) {
		powerupTarget = false;
		// find direction with maximum frames until collision

		/* TODO changed from 0 to 1 to disable hold position
		 * note: there is a major problem with how we do things.
		 * We do not know the velocities of all entities (e.g. enemies, lasers),
		 * so we assume them to have zero velocity.
		 * Therefore the hold position grace time may be overestimated significantly,
		 * especially if the hazard is heading directly towards the player.
		 * Thus, we disable the hold position. This would be fine, but causes the player
		 * to constantly spaz because it cannot stay still. Then, the gameplay does not look
		 * realistic.
		 */
		int maxIdx = 1;

		// HACK if we are not being threatened by any bullets, then allow hold position
		if (bounded)
		{
			maxIdx = 0;
		}

		for (int dir = 1; dir < control::Movement::MaxValue; ++dir)
		{
			if (collisionTicks[dir] != FLT_MAX &&
				collisionTicks[dir] > collisionTicks[maxIdx])
			{
				maxIdx = dir;
			}
		}
		tarIdx = maxIdx;
	}

	/* IMGUI Integration */
	int minTimeIdx = 0;
	for (int dir = 1; dir < control::Movement::MaxValue; ++dir)
	{
		if (collisionTicks[dir] < collisionTicks[minTimeIdx])
			minTimeIdx = dir;
	}
	for (int i = 1; i < RISK_HISTORY_SIZE; ++i)
		riskHistory[i - 1] = riskHistory[i];
	riskHistory[RISK_HISTORY_SIZE - 1] = collisionTicks[minTimeIdx];
	PlotLines("danger hist", riskHistory, IM_ARRAYSIZE(riskHistory), 0, "",
		0.f, 30.f, ImVec2(0, 80));
	SameLine(); ShowHelpMarker("frames until collision of the best move,\n"
		"maximization parameter");

	/*LOG("C[%d] | H:%.0f U:%.0f D:%.0f L:%.0f R:%.0f UL:%.0f UR:%.0f DL:%.0f DR:%.0f",
		tarIdx,
		collisionTicks[0] == FLT_MAX ? -1 : collisionTicks[0], collisionTicks[1],
		collisionTicks[2], collisionTicks[3], collisionTicks[4],
		collisionTicks[5], collisionTicks[6], collisionTicks[7], collisionTicks[8]
	);*/

	di8->setVkState(DIK_Z, DIK_KEY_DOWN);			// fire continuously
	di8->setVkState(DIK_LCONTROL, DIK_KEY_DOWN);	// skip dialogue continuously

	// release all control keys
	for (int x : control::kControlKeys)
		di8->resetVkState(x);

	// press required keys for moving in desired direction
	for (int i = 0; i < 3; ++i) {
		if (control::kMovementToInput[tarIdx][i])
			di8->setVkState(control::kMovementToInput[tarIdx][i], DIK_KEY_DOWN);
	}

	// deathbomb if the bot is going to die in the next frame
	// this is very dependent on the collision predictor being very accurate
	if (collisionTicks[tarIdx] < 0.5f)
	{
		di8->setVkState(DIK_X, DIK_KEY_DOWN);
	}

	Checkbox("Show Vector Field", &this->renderVectorField);

	End();

}

void th_vo_algo::calibInit()
{
	isCalibrated = false;
	calibFrames = 0;
	calibStartX = -1;
	playerVel = 0;
}

vec2 th_vo_algo::getPlayerMovement(int dir)
{
	return control::kMovementVelocity[dir] * (control::kMovementFocused[dir] ? playerFocVel : playerVel);
}

float th_vo_algo::minStaticCollideTick(
	const std::vector<const game_object*>& bullets,
	const aabb& area,
	std::vector<const game_object*>& collided) const
{
	float minTick = FLT_MAX;
	for (const game_object* bullet : bullets)
	{
		float colTick = area.willCollideWith(*bullet->obj);

		if (colTick >= 0) {
			minTick = std::min(colTick, minTick);
			collided.push_back(bullet);
		}
	}
	if (minTick != FLT_MAX && minTick >= 0)
		return minTick;
	return -1.f;
};

void th_vo_algo::vizPotentialQuadtree(
	const std::vector<const game_object*>& bullets,
	const aabb& area,
	float minRes) const
{
	/*cdraw::rect(
		th_param.GAME_X_OFFSET + p.x, th_param.GAME_Y_OFFSET + p.y,
		s.x, s.y,
		D3DCOLOR_ARGB(80, 255, 255, 0)
	);*/

	// create four square regions of equal size which contain (p, s)
	// they are square, since we want the final pixels to be square
	vec2 center = area.com();
	float fSqsz = std::max(area.size.w, area.size.h) / 2;

	// not necessary, just a safety net
	if (fSqsz < minRes) {
		return;
	}
	vec2 sqsz(fSqsz, fSqsz);

	vec2 colDomains[] = {
		center - sqsz,							// top-left
		vec2(center.x, center.y - sqsz.y),		// top-right
		vec2(center.x - sqsz.x, center.y),		// bottom-left
		center									// bottom-right
	};

	for (int i = 0; i < 4; i++)
	{
		std::vector<const game_object*> collided;
		float colTick = minStaticCollideTick(
			bullets,
			aabb{ colDomains[i], vec2(), vec2(sqsz) },
			collided);
		if (colTick >= 0) {
			if (fSqsz / 2 <= minRes)
			{
				float fadeCoeff = std::max(0.0f, std::min(1.0f, 1.0f / (colTick / MAX_FRAMES_TILL_COLLISION)));
				hsv col_hsv = { 0, fadeCoeff,  fadeCoeff };
				rgb col_rgb = hsv2rgb(col_hsv);
				cdraw::fillRect(
					th_param.GAME_X_OFFSET + colDomains[i].x,
					th_param.GAME_Y_OFFSET + colDomains[i].y,
					sqsz.x, sqsz.y,
					D3DCOLOR_ARGB((int)(fadeCoeff * 128),
					(int)(col_rgb.r * 255), (int)(col_rgb.g * 255), (int)(col_rgb.b * 255))
				);
			}
			else {
				vizPotentialQuadtree(
					collided,
					aabb{ colDomains[i], vec2(), vec2(sqsz) },
					minRes);
			}
		}
	}

}

std::vector<const game_object*> th_vo_algo::constructDangerObjectUnion()
{
	std::vector<const game_object*> objs;
	for (const laser& l : player->lasers)
		objs.push_back(&l);
	for (const bullet& b : player->bullets)
		objs.push_back(&b);
	for (const enemy& e : player->enemies)
		objs.push_back(&e);
	return objs;
}

void th_vo_algo::visualize(IDirect3DDevice9* d3dDev)
{
	if (player->render)
	{
		auto plyr = player->getPlayerEntity();

		if (this->renderVectorField)
		{
			// draw vector field (laggy)
			vizPotentialQuadtree(
				constructDangerObjectUnion(),
				aabb{ vec2(), vec2(), vec2(th_param.GAME_WIDTH, th_param.GAME_HEIGHT) },
				VEC_FIELD_MIN_RESOLUTION);
		}
		for (const laser& l : player->lasers)
			l.render();
		for (const bullet& b : player->bullets)
			b.render();
		for (const enemy& e : player->enemies)
			e.render();
		for (const powerup& p : player->powerups)
			p.render();
		plyr.render();

	}
}

bool th_vo_algo::calibTick()
{
	auto plyr = player->getPlayerEntity();

	switch (calibFrames)
	{
	case 0:
		// do not allow player interaction during calibration
		th_di8_hook::inst()->setVkState(DIK_LEFT, DIK_KEY_DOWN);
		th_di8_hook::inst()->setVkState(DIK_RIGHT, DIK_KEY_UP);
		th_di8_hook::inst()->setVkState(DIK_UP, DIK_KEY_UP);
		th_di8_hook::inst()->setVkState(DIK_DOWN, DIK_KEY_UP);
		break;
	case 1:
		th_di8_hook::inst()->setVkState(DIK_LEFT, DIK_KEY_UP);
		calibStartX = plyr.obj->com().x;
		break;
	case 2:
		th_di8_hook::inst()->setVkState(DIK_RIGHT, DIK_KEY_DOWN);
		break;
	case 3:
		th_di8_hook::inst()->resetVkState(DIK_LEFT);
		th_di8_hook::inst()->resetVkState(DIK_RIGHT);
		th_di8_hook::inst()->resetVkState(DIK_UP);
		th_di8_hook::inst()->resetVkState(DIK_DOWN);

		// BUG why does LoLK do this differently
		if (dynamic_cast<th15_player*>(player)
			|| dynamic_cast<th10_player*>(player)
			|| dynamic_cast<th11_player*>(player))
			playerVel = plyr.obj->com().x - calibStartX;
		else
			playerVel = calibStartX - plyr.obj->com().x;
		break;
	case 4:
		// do not allow player interaction during calibration
		th_di8_hook::inst()->setVkState(DIK_LEFT, DIK_KEY_DOWN);
		th_di8_hook::inst()->setVkState(DIK_RIGHT, DIK_KEY_UP);
		th_di8_hook::inst()->setVkState(DIK_UP, DIK_KEY_UP);
		th_di8_hook::inst()->setVkState(DIK_DOWN, DIK_KEY_UP);
		th_di8_hook::inst()->setVkState(DIK_LSHIFT, DIK_KEY_DOWN);
		break;
	case 5:
		th_di8_hook::inst()->setVkState(DIK_LEFT, DIK_KEY_UP);
		calibStartX = plyr.obj->com().x;
		break;
	case 6:
		th_di8_hook::inst()->setVkState(DIK_RIGHT, DIK_KEY_DOWN);
		break;
	case 7:
		isCalibrated = true;
		th_di8_hook::inst()->resetVkState(DIK_LEFT);
		th_di8_hook::inst()->resetVkState(DIK_RIGHT);
		th_di8_hook::inst()->resetVkState(DIK_UP);
		th_di8_hook::inst()->resetVkState(DIK_DOWN);
		th_di8_hook::inst()->resetVkState(DIK_LSHIFT);

		// BUG why do MoF and LoLK do this differently
		if (dynamic_cast<th15_player*>(player)
			|| dynamic_cast<th10_player*>(player)
			|| dynamic_cast<th11_player*>(player))
			playerFocVel = plyr.obj->com().x - calibStartX;
		else
			playerFocVel = calibStartX - plyr.obj->com().x;
		return true;
	}
	++calibFrames;
	return false;
}





