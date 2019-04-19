/* Copyright (C) 2016, Nikolai Wuttke. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "game_runner.hpp"

#include "data/game_traits.hpp"
#include "data/map.hpp"
#include "data/sound_ids.hpp"
#include "data/strings.hpp"
#include "data/unit_conversions.hpp"
#include "engine/physical_components.hpp"
#include "game_logic/actor_tag.hpp"
#include "game_logic/ingame_systems.hpp"
#include "game_logic/trigger_components.hpp"
#include "loader/resource_loader.hpp"
#include "ui/menu_element_renderer.hpp"
#include "ui/utils.hpp"

#include "game_service_provider.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <sstream>


namespace rigel {

using namespace engine;
using namespace game_logic;
using namespace std;

using data::PlayerModel;
using engine::components::WorldPosition;


namespace {

// Update game logic at 15 FPS. This is not exactly the speed at which the
// game runs on period-appropriate hardware, but it's very close, and it nicely
// fits into 60 FPS, giving us 4 render frames for 1 logic update.
//
// On a 486 with a fast graphics card, the game runs at roughly 15.5 FPS, with
// a slower (non-VLB) graphics card, it's roughly 14 FPS. On a fast 386 (40 MHz),
// it's roughly 13 FPS. With 15 FPS, the feel should therefore be very close to
// playing the game on a 486 at the default game speed setting.
constexpr auto GAME_LOGIC_UPDATE_DELAY = 1.0/15.0;

char EPISODE_PREFIXES[] = {'L', 'M', 'N', 'O'};


std::string levelFileName(const int episode, const int level) {
  assert(episode >=0 && episode < 4);
  assert(level >=0 && level < 8);

  std::string fileName;
  fileName += EPISODE_PREFIXES[episode];
  fileName += std::to_string(level + 1);
  fileName += ".MNI";
  return fileName;
}


constexpr auto BOSS_LEVEL_INTRO_MUSIC = "CALM.IMF";


struct BonusRelatedItemCounts {
  int mCameraCount = 0;
  int mFireBombCount = 0;
  int mWeaponCount = 0;
  int mMerchandiseCount = 0;
  int mBonusGlobeCount = 0;
  int mLaserTurretCount = 0;
};


BonusRelatedItemCounts countBonusRelatedItems(entityx::EntityManager& es) {
  using game_logic::components::ActorTag;
  using AT = ActorTag::Type;

  BonusRelatedItemCounts counts;

  es.each<ActorTag>([&counts](entityx::Entity, const ActorTag& tag) {
      switch (tag.mType) {
        case AT::ShootableCamera: ++counts.mCameraCount; break;
        case AT::FireBomb: ++counts.mFireBombCount; break;
        case AT::CollectableWeapon: ++counts.mWeaponCount; break;
        case AT::Merchandise: ++counts.mMerchandiseCount; break;
        case AT::ShootableBonusGlobe: ++counts.mBonusGlobeCount; break;
        case AT::MountedLaserTurret: ++counts.mLaserTurretCount; break;

        default:
          break;
      }
    });

  return counts;
}


constexpr auto HEALTH_BAR_LABEL_START_X = 1;
constexpr auto HEALTH_BAR_LABEL_START_Y = 0;
constexpr auto HEALTH_BAR_TILE_INDEX = 4*40 + 1;
constexpr auto HEALTH_BAR_START_PX = base::Vector{data::tilesToPixels(6), 0};


void drawBossHealthBar(
  const int health,
  const ui::MenuElementRenderer& textRenderer,
  const engine::TileRenderer& uiSpriteSheet
) {
  textRenderer.drawSmallWhiteText(
    HEALTH_BAR_LABEL_START_X, HEALTH_BAR_LABEL_START_Y, "BOSS");

  const auto healthBarSize = base::Extents{health, data::GameTraits::tileSize};
  uiSpriteSheet.renderTileStretched(
    HEALTH_BAR_TILE_INDEX, {HEALTH_BAR_START_PX, healthBarSize});
}

}


GameWorld::GameWorld(
  data::PlayerModel* pPlayerModel,
  const data::GameSessionId& sessionId,
  GameMode::Context context,
  std::optional<base::Vector> playerPositionOverride,
  bool showWelcomeMessage
)
  : mpRenderer(context.mpRenderer)
  , mpServiceProvider(context.mpServiceProvider)
  , mpUiSpriteSheet(context.mpUiSpriteSheetRenderer)
  , mpTextRenderer(context.mpUiRenderer)
  , mEntities(mEventManager)
  , mEntityFactory(
      context.mpRenderer,
      &mEntities,
      &context.mpResources->mActorImagePackage,
      sessionId.mDifficulty)
  , mpPlayerModel(pPlayerModel)
  , mPlayerModelAtLevelStart(*mpPlayerModel)
  , mRadarDishCounter(mEntities, mEventManager)
  , mHudRenderer(
      mpPlayerModel,
      sessionId.mLevel + 1,
      mpRenderer,
      *context.mpResources,
      context.mpUiSpriteSheetRenderer)
  , mMessageDisplay(mpServiceProvider, context.mpUiRenderer)
{
  mEventManager.subscribe<rigel::events::CheckPointActivated>(*this);
  mEventManager.subscribe<rigel::events::ExitReached>(*this);
  mEventManager.subscribe<rigel::events::PlayerDied>(*this);
  mEventManager.subscribe<rigel::events::PlayerTookDamage>(*this);
  mEventManager.subscribe<rigel::events::PlayerMessage>(*this);
  mEventManager.subscribe<rigel::events::PlayerTeleported>(*this);
  mEventManager.subscribe<rigel::events::ScreenFlash>(*this);
  mEventManager.subscribe<rigel::events::ScreenShake>(*this);
  mEventManager.subscribe<rigel::events::TutorialMessage>(*this);
  mEventManager.subscribe<rigel::game_logic::events::ShootableKilled>(*this);
  mEventManager.subscribe<rigel::events::BossActivated>(*this);

  using namespace std::chrono;
  auto before = high_resolution_clock::now();

  loadLevel(sessionId, *context.mpResources);

  if (playerPositionOverride) {
    mpSystems->player().position() = *playerPositionOverride;
  }

  mpSystems->centerViewOnPlayer();

  updateGameLogic({});

  if (showWelcomeMessage) {
    mMessageDisplay.setMessage(data::Messages::WelcomeToDukeNukem2);
  }

  if (mEarthQuakeEffect) { // overrides welcome message
    showTutorialMessage(data::TutorialMessageId::EarthQuake);
  }

  if (mRadarDishCounter.radarDishesPresent()) { // overrides earth quake
    mMessageDisplay.setMessage(data::Messages::FindAllRadars);
  }

  auto after = high_resolution_clock::now();
  std::cout << "Level load time: " <<
    duration<double>(after - before).count() * 1000.0 << " ms\n";
}


GameWorld::~GameWorld() = default;


bool GameWorld::levelFinished() const {
  return mLevelFinished;
}


std::set<data::Bonus> GameWorld::achievedBonuses() const {
  std::set<data::Bonus> bonuses;

  if (!mBonusInfo.mPlayerTookDamage) {
    bonuses.insert(data::Bonus::NoDamageTaken);
  }

  const auto counts =
    countBonusRelatedItems(const_cast<entityx::EntityManager&>(mEntities));

  if (mBonusInfo.mInitialCameraCount > 0 && counts.mCameraCount == 0) {
    bonuses.insert(data::Bonus::DestroyedAllCameras);
  }

  // NOTE: This is a bug (?) in the original game - if a level doesn't contain
  // any fire bombs, bonus 6 will be awarded, as if the player had destroyed
  // all fire bombs.
  if (counts.mFireBombCount == 0) {
    bonuses.insert(data::Bonus::DestroyedAllFireBombs);
  }

  if (
    mBonusInfo.mInitialMerchandiseCount > 0 && counts.mMerchandiseCount == 0
  ) {
    bonuses.insert(data::Bonus::CollectedAllMerchandise);
  }

  if (mBonusInfo.mInitialWeaponCount > 0 && counts.mWeaponCount == 0) {
    bonuses.insert(data::Bonus::CollectedEveryWeapon);
  }

  if (
    mBonusInfo.mInitialLaserTurretCount > 0 && counts.mLaserTurretCount == 0
  ) {
    bonuses.insert(data::Bonus::DestroyedAllSpinningLaserTurrets);
  }

  if (mBonusInfo.mInitialBonusGlobeCount == mBonusInfo.mNumShotBonusGlobes) {
    bonuses.insert(data::Bonus::ShotAllBonusGlobes);
  }

  return bonuses;
}


void GameWorld::receive(const rigel::events::CheckPointActivated& event) {
  mActivatedCheckpoint = CheckpointData{
    mpPlayerModel->makeCheckpoint(), event.mPosition};
  mMessageDisplay.setMessage(data::Messages::FoundRespawnBeacon);
}


void GameWorld::receive(const rigel::events::ExitReached& event) {
  if (mRadarDishCounter.radarDishesPresent() && event.mCheckRadarDishes) {
    showTutorialMessage(data::TutorialMessageId::RadarsStillFunctional);
  } else {
    mLevelFinished = true;
  }
}


void GameWorld::receive(const rigel::events::PlayerDied& event) {
  mPlayerDied = true;
}


void GameWorld::receive(const rigel::events::PlayerTookDamage& event) {
  mBonusInfo.mPlayerTookDamage = true;
}


void GameWorld::receive(const rigel::events::PlayerMessage& event) {
  mMessageDisplay.setMessage(event.mText);
}


void GameWorld::receive(const rigel::events::PlayerTeleported& event) {
  mTeleportTargetPosition = event.mNewPosition;
}


void GameWorld::receive(const rigel::events::ScreenFlash& event) {
  mScreenFlashColor = event.mColor;
}


void GameWorld::receive(const rigel::events::ScreenShake& event) {
  mScreenShakeOffsetX = event.mAmount;
}


void GameWorld::receive(const rigel::events::TutorialMessage& event) {
  showTutorialMessage(event.mId);
}


void GameWorld::receive(const game_logic::events::ShootableKilled& event) {
  using game_logic::components::ActorTag;
  auto entity = event.mEntity;
  if (!entity.has_component<ActorTag>()) {
    return;
  }

  const auto& position = *entity.component<const WorldPosition>();

  using AT = ActorTag::Type;
  const auto type = entity.component<ActorTag>()->mType;
  switch (type) {
    case AT::Reactor:
      onReactorDestroyed(position);
      break;

    case AT::ShootableBonusGlobe:
      ++mBonusInfo.mNumShotBonusGlobes;
      break;

    default:
      break;
  }
}


void GameWorld::receive(const rigel::events::BossActivated& event) {
  mActiveBossEntity = event.mBossEntity;
  mpServiceProvider->playMusic(*mLevelMusicFile);
}


void GameWorld::loadLevel(
  const data::GameSessionId& sessionId,
  const loader::ResourceLoader& resources
) {
  auto loadedLevel = loader::loadLevel(
    levelFileName(sessionId.mEpisode, sessionId.mLevel),
    resources,
    sessionId.mDifficulty);
  auto playerEntity =
    mEntityFactory.createEntitiesForLevel(loadedLevel.mActors);

  const auto counts = countBonusRelatedItems(mEntities);
  mBonusInfo.mInitialCameraCount = counts.mCameraCount;
  mBonusInfo.mInitialMerchandiseCount = counts.mMerchandiseCount;
  mBonusInfo.mInitialWeaponCount = counts.mWeaponCount;
  mBonusInfo.mInitialLaserTurretCount = counts.mLaserTurretCount;
  mBonusInfo.mInitialBonusGlobeCount = counts.mBonusGlobeCount;

  mLevelData = LevelData{
    std::move(loadedLevel.mMap),
    std::move(loadedLevel.mActors),
    loadedLevel.mBackdropSwitchCondition
  };
  mMapAtLevelStart = mLevelData.mMap;

  mpSystems = std::make_unique<IngameSystems>(
    sessionId,
    playerEntity,
    mpPlayerModel,
    &mLevelData.mMap,
    engine::MapRenderer::MapRenderData{std::move(loadedLevel)},
    mpServiceProvider,
    &mEntityFactory,
    &mRandomGenerator,
    &mRadarDishCounter,
    mpRenderer,
    mEntities,
    mEventManager,
    resources);

  if (loadedLevel.mEarthquake) {
    mEarthQuakeEffect = engine::EarthQuakeEffect{
      mpServiceProvider, &mRandomGenerator, &mEventManager};
  }

  if (data::isBossLevel(sessionId.mLevel)) {
    mLevelMusicFile = loadedLevel.mMusicFile;
    mpServiceProvider->playMusic(BOSS_LEVEL_INTRO_MUSIC);
  } else {
    mpServiceProvider->playMusic(loadedLevel.mMusicFile);
  }
}


void GameWorld::updateGameLogic(const PlayerInput& input) {
  mBackdropFlashColor = std::nullopt;
  mScreenFlashColor = std::nullopt;

  if (mReactorDestructionFramesElapsed) {
    updateReactorDestructionEvent();
  }

  if (mEarthQuakeEffect) {
    mEarthQuakeEffect->update();
  }

  mHudRenderer.updateAnimation();
  mMessageDisplay.update();

  mpSystems->update(input, mEntities);
}


void GameWorld::render() {
  mpRenderer->clear();

  {
    Renderer::StateSaver saveState(mpRenderer);
    mpRenderer->setClipRect(base::Rect<int>{
      {data::GameTraits::inGameViewPortOffset.x + mScreenShakeOffsetX,
      data::GameTraits::inGameViewPortOffset.y},
      {data::GameTraits::inGameViewPortSize.width,
      data::GameTraits::inGameViewPortSize.height}});

    mpRenderer->setGlobalTranslation({
      data::GameTraits::inGameViewPortOffset.x + mScreenShakeOffsetX,
      data::GameTraits::inGameViewPortOffset.y});

    if (!mScreenFlashColor) {
      mpSystems->render(mEntities, mBackdropFlashColor);
    } else {
      mpRenderer->clear(*mScreenFlashColor);
    }
    mHudRenderer.render();
  }

  if (mActiveBossEntity) {
    using game_logic::components::Shootable;

    const auto health = mActiveBossEntity.has_component<Shootable>()
      ? mActiveBossEntity.component<Shootable>()->mHealth : 0;
    drawBossHealthBar(health, *mpTextRenderer, *mpUiSpriteSheet);
  } else {
    mMessageDisplay.render();
  }
}


void GameWorld::processEndOfFrameActions() {
  handlePlayerDeath();
  handleLevelExit();
  handleTeleporter();

  mScreenShakeOffsetX = 0;
}


void GameWorld::onReactorDestroyed(const base::Vector& position) {
  mScreenFlashColor = loader::INGAME_PALETTE[7];
  mEntityFactory.createProjectile(
    ProjectileType::ReactorDebris,
    position + base::Vector{-1, 0},
    ProjectileDirection::Left);
  mEntityFactory.createProjectile(
    ProjectileType::ReactorDebris,
    position + base::Vector{3, 0},
    ProjectileDirection::Right);

  const auto shouldDoSpecialEvent =
    mLevelData.mBackdropSwitchCondition ==
      data::map::BackdropSwitchCondition::OnReactorDestruction;
  if (!mReactorDestructionFramesElapsed && shouldDoSpecialEvent) {
    mpSystems->switchBackdrops();
    mBackdropSwitched = true;
    mReactorDestructionFramesElapsed = 0;
  }
}


void GameWorld::updateReactorDestructionEvent() {
  auto& framesElapsed = *mReactorDestructionFramesElapsed;
  if (framesElapsed >= 14) {
    return;
  }

  if (framesElapsed == 13) {
    mMessageDisplay.setMessage(data::Messages::DestroyedEverything);
  } else if (framesElapsed % 2 == 1) {
    mBackdropFlashColor = base::Color{255, 255, 255, 255};
    mpServiceProvider->playSound(data::SoundId::BigExplosion);
  }

  ++framesElapsed;
}


void GameWorld::handleLevelExit() {
  using engine::components::Active;
  using engine::components::BoundingBox;
  using game_logic::components::Trigger;
  using game_logic::components::TriggerType;

  mEntities.each<Trigger, WorldPosition, Active>(
    [this](
      entityx::Entity,
      const Trigger& trigger,
      const WorldPosition& triggerPosition,
      const Active&
    ) {
      if (trigger.mType != TriggerType::LevelExit || mLevelFinished) {
        return;
      }

      const auto playerBBox = mpSystems->player().worldSpaceHitBox();
      const auto playerAboveOrAtTriggerHeight =
        playerBBox.bottom() <= triggerPosition.y;
      const auto touchingTriggerOnXAxis =
        triggerPosition.x >= playerBBox.left() &&
        triggerPosition.x <= (playerBBox.right() + 1);

      const auto triggerActivated =
        playerAboveOrAtTriggerHeight && touchingTriggerOnXAxis;

      if (triggerActivated) {
        mEventManager.emit(rigel::events::ExitReached{});
      }
    });
}


void GameWorld::handlePlayerDeath() {
  if (mPlayerDied) {
    mPlayerDied = false;

    if (mActivatedCheckpoint) {
      restartFromCheckpoint();
    } else {
      restartLevel();
    }
  }
}


void GameWorld::restartLevel() {
  mpServiceProvider->fadeOutScreen();

  if (mBackdropSwitched) {
    mpSystems->switchBackdrops();
    mBackdropSwitched = false;
  }

  mLevelData.mMap = mMapAtLevelStart;

  mEntities.reset();
  auto playerEntity = mEntityFactory.createEntitiesForLevel(
    mLevelData.mInitialActors);
  mpSystems->restartFromBeginning(playerEntity);

  *mpPlayerModel = mPlayerModelAtLevelStart;

  mpSystems->centerViewOnPlayer();
  render();

  mpServiceProvider->fadeInScreen();
}


void GameWorld::restartFromCheckpoint() {
  mpServiceProvider->fadeOutScreen();

  const auto shouldSwitchBackAfterRespawn =
    mLevelData.mBackdropSwitchCondition ==
      data::map::BackdropSwitchCondition::OnTeleportation;
  if (mBackdropSwitched && shouldSwitchBackAfterRespawn) {
    mpSystems->switchBackdrops();
    mBackdropSwitched = false;
  }

  mpPlayerModel->restoreFromCheckpoint(mActivatedCheckpoint->mState);
  mpSystems->restartFromCheckpoint(mActivatedCheckpoint->mPosition);

  mpSystems->centerViewOnPlayer();
  render();

  mpServiceProvider->fadeInScreen();
}


void GameWorld::handleTeleporter() {
  if (!mTeleportTargetPosition) {
    return;
  }

  mpServiceProvider->fadeOutScreen();

  mpSystems->player().position() = *mTeleportTargetPosition;
  mTeleportTargetPosition = std::nullopt;

  const auto switchBackdrop =
    mLevelData.mBackdropSwitchCondition ==
      data::map::BackdropSwitchCondition::OnTeleportation;
  if (switchBackdrop) {
    mpSystems->switchBackdrops();
    mBackdropSwitched = !mBackdropSwitched;
  }

  mpSystems->centerViewOnPlayer();
  render();
  mpServiceProvider->fadeInScreen();
}


void GameWorld::showTutorialMessage(const data::TutorialMessageId id) {
  if (!mpPlayerModel->tutorialMessages().hasBeenShown(id)) {
    mMessageDisplay.setMessage(data::messageText(id));
    mpPlayerModel->tutorialMessages().markAsShown(id);
  }
}


void GameWorld::showDebugText() {
  std::stringstream infoText;
  mpSystems->printDebugText(infoText);
  infoText
    << "Entities: " << mEntities.size();

  mpServiceProvider->showDebugText(infoText.str());
}


GameRunner::GameRunner(
  data::PlayerModel* pPlayerModel,
  const data::GameSessionId& sessionId,
  GameMode::Context context,
  const std::optional<base::Vector> playerPositionOverride,
  const bool showWelcomeMessage
)
  : mWorld(
      pPlayerModel,
      sessionId,
      context,
      playerPositionOverride,
      showWelcomeMessage)
{
}


void GameRunner::handleEvent(const SDL_Event& event) {
  const auto isKeyEvent = event.type == SDL_KEYDOWN || event.type == SDL_KEYUP;
  if (!isKeyEvent || event.key.repeat != 0) {
    return;
  }

  const auto keyPressed = std::uint8_t{event.type == SDL_KEYDOWN};
  switch (event.key.keysym.sym) {
    // TODO: DRY this up a little?
    case SDLK_UP:
      mPlayerInput.mUp = keyPressed;
      mPlayerInput.mInteract.mIsPressed = keyPressed;
      if (keyPressed) {
        mPlayerInput.mInteract.mWasTriggered = true;
      }
      break;

    case SDLK_DOWN:
      mPlayerInput.mDown = keyPressed;
      break;

    case SDLK_LEFT:
      mPlayerInput.mLeft = keyPressed;
      break;

    case SDLK_RIGHT:
      mPlayerInput.mRight = keyPressed;
      break;

    case SDLK_LCTRL:
    case SDLK_RCTRL:
      mPlayerInput.mJump.mIsPressed = keyPressed;
      if (keyPressed) {
        mPlayerInput.mJump.mWasTriggered = true;
      }
      break;

    case SDLK_LALT:
    case SDLK_RALT:
      mPlayerInput.mFire.mIsPressed = keyPressed;
      if (keyPressed) {
        mPlayerInput.mFire.mWasTriggered = true;
      }
      break;
  }

  // Debug keys
  // ----------------------------------------------------------------------
  if (keyPressed) {
    return;
  }

  auto& debuggingSystem = mWorld.mpSystems->debuggingSystem();
  switch (event.key.keysym.sym) {
    case SDLK_b:
      debuggingSystem.toggleBoundingBoxDisplay();
      break;

    case SDLK_c:
      debuggingSystem.toggleWorldCollisionDataDisplay();
      break;

    case SDLK_d:
      mShowDebugText = !mShowDebugText;
      break;

    case SDLK_g:
      debuggingSystem.toggleGridDisplay();
      break;

    case SDLK_s:
      mSingleStepping = !mSingleStepping;
      break;

    case SDLK_SPACE:
      if (mSingleStepping) {
        mDoNextSingleStep = true;
      }
      break;
  }
}


void GameRunner::updateAndRender(engine::TimeDelta dt) {
  if (mWorld.levelFinished()) {
    return;
  }

  // **********************************************************************
  // Updating
  // **********************************************************************

  auto update = [this]() {
    mWorld.updateGameLogic(mPlayerInput);
    mPlayerInput.resetTriggeredStates();
  };

  if (mSingleStepping) {
    if (mDoNextSingleStep) {
      update();
      mDoNextSingleStep = false;
    }
  } else {
    mAccumulatedTime += dt;
    for (;
      mAccumulatedTime >= GAME_LOGIC_UPDATE_DELAY;
      mAccumulatedTime -= GAME_LOGIC_UPDATE_DELAY
    ) {
      update();
    }
  }


  // **********************************************************************
  // Rendering
  // **********************************************************************
  mWorld.render();

  if (mShowDebugText) {
    mWorld.showDebugText();
  }

  mWorld.processEndOfFrameActions();
}

}
