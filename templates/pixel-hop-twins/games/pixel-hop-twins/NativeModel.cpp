// Native translation of Pixel Hop Twins by stm1978, revision 9835295888c7.
// The MIT-licensed source update order and double-precision arithmetic are retained.
#include "NativeModel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(__clang__)
// JavaScript uses separate double operations. Preserve that order on ARM64.
#pragma clang fp contract(off)
#endif

namespace PixelHop {
namespace {
constexpr double kGravity = 1400;
constexpr double kMaxFallSpeed = 800;
constexpr double kCoyoteTime = 0.1;
constexpr double kJumpBufferTime = 0.1;
constexpr double kWalkSpeed = 220;
constexpr double kWalkAccel = 1800;
constexpr double kFriction = 1400;
constexpr double kJumpVelocity = -560;
constexpr double kJumpCut = 0.45;
constexpr double kDoubleJumpVelocity = -480;

int TileCoordinate(double pixel) {
    return static_cast<int>(std::floor(pixel / kTileSize));
}

std::vector<Collectible> ReadCollectibles(const nlohmann::json& data, const char* key) {
    std::vector<Collectible> result;
    if (!data.contains(key) || data.at(key).is_null()) return result;
    for (const auto& item : data.at(key)) {
        Collectible collectible;
        collectible.x = item.at("x").get<double>();
        collectible.y = item.at("y").get<double>();
        result.push_back(collectible);
    }
    return result;
}
} // namespace

Level::Level(const nlohmann::json& data) {
    if (data.contains("name") && !data.at("name").is_null()) name = data.at("name").get<std::string>();
    if (data.contains("timeLimit") && !data.at("timeLimit").is_null()) timeLimit = data.at("timeLimit").get<double>();
    tiles = data.at("tiles").get<std::vector<std::vector<int>>>();
    if (tiles.empty() || tiles.front().empty()) throw std::runtime_error("Pixel Hop Twins level has no tiles");
    rows = static_cast<int>(tiles.size());
    cols = static_cast<int>(tiles.front().size());
    for (const auto& row : tiles) {
        if (row.size() != tiles.front().size()) throw std::runtime_error("Pixel Hop Twins level has inconsistent row lengths");
    }
    gems = ReadCollectibles(data, "gems");
    powerUps = ReadCollectibles(data, "powerUps");
    playerStart = {1, static_cast<double>(rows - 3)};
    if (data.contains("playerStart") && !data.at("playerStart").is_null()) {
        playerStart = {data.at("playerStart").at("x").get<double>(), data.at("playerStart").at("y").get<double>()};
    }
    if (data.contains("enemies") && !data.at("enemies").is_null()) {
        for (const auto& item : data.at("enemies")) {
            EnemySpawn spawn;
            spawn.x = item.at("x").get<double>();
            spawn.y = item.at("y").get<double>();
            if (item.contains("type") && !item.at("type").is_null()) spawn.type = item.at("type").get<std::string>();
            if (item.contains("speed") && !item.at("speed").is_null()) spawn.speed = item.at("speed").get<double>();
            enemies.push_back(spawn);
        }
    }
}

int Level::TileAt(int col, int row) const {
    if (row < 0 || row >= rows || col < 0 || col >= cols) return 0;
    return tiles[row][col];
}

bool Level::IsSolid(int col, int row) const { return TileAt(col, row) == 1; }
bool Level::IsPlatform(int col, int row) const { return TileAt(col, row) == 2; }
bool Level::IsSpike(int col, int row) const { return TileAt(col, row) == 3; }
bool Level::IsFloor(int col, int row) const { return IsSolid(col, row) || IsPlatform(col, row); }
void Level::Update(double dt) { _t += dt; }

Camera::Camera(int levelCols, int levelRows)
    : _levelW(levelCols * kTileSize), _levelH(levelRows * kTileSize) {}

void Camera::Update(double targetX, double targetY, double dt) {
    const double desiredX = targetX - kCanvasWidth / 2;
    const double desiredY = targetY - kCanvasHeight / 2;
    x += (desiredX - x) * 8 * dt;
    y += (desiredY - y) * 8 * dt;
    x = std::max(0.0, std::min(x, _levelW - kCanvasWidth));
    y = std::max(0.0, std::min(y, _levelH - kCanvasHeight));
}

Player::Player(double startX, double startY) : x(startX), y(startY) {}
double Player::CenterX() const { return x + w / 2; }
double Player::CenterY() const { return y + h / 2; }
double Player::Right() const { return x + w; }
double Player::Bottom() const { return y + h; }

void Player::ActivatePrisma(SoundEvents& sounds) {
    prismaMode = true;
    _prismaTimer = 15;
    _doubleJumpUsed = false;
    sounds.push_back("powerup");
}

void Player::DeactivatePrisma() {
    prismaMode = false;
    _prismaTimer = 0;
    _doubleJumpUsed = false;
}

void Player::Update(double dt, const Level& level, const InputFrame& input, SoundEvents& sounds) {
    if (!alive) return;
    if (prismaMode) {
        _prismaTimer -= dt;
        if (_prismaTimer <= 0) DeactivatePrisma();
    }
    if (_iframes > 0) _iframes -= dt;

    if (input.left && !input.right) {
        vx -= kWalkAccel * dt;
        if (vx < -kWalkSpeed) vx = -kWalkSpeed;
        _facingLeft = true;
    } else if (input.right && !input.left) {
        vx += kWalkAccel * dt;
        if (vx > kWalkSpeed) vx = kWalkSpeed;
        _facingLeft = false;
    } else {
        if (vx > 0) {
            vx -= kFriction * dt;
            if (vx < 0) vx = 0;
        } else if (vx < 0) {
            vx += kFriction * dt;
            if (vx > 0) vx = 0;
        }
    }
    if (input.jumpPressed) _jumpBuf = kJumpBufferTime;
    else _jumpBuf = std::max(0.0, _jumpBuf - dt);

    if (onGround) {
        _coyote = kCoyoteTime;
        _doubleJumpUsed = false;
    } else {
        _coyote = std::max(0.0, _coyote - dt);
    }
    vy += kGravity * dt;
    if (vy > kMaxFallSpeed) vy = kMaxFallSpeed;
    if (_jumpBuf > 0) {
        if (_coyote > 0) {
            vy = kJumpVelocity;
            _coyote = 0;
            _jumpBuf = 0;
            sounds.push_back("jump");
        } else if (prismaMode && !_doubleJumpUsed && !onGround) {
            vy = kDoubleJumpVelocity;
            _doubleJumpUsed = true;
            _jumpBuf = 0;
            sounds.push_back("jump");
        }
    }
    if (input.jumpReleased && vy < 0) vy *= kJumpCut;

    x += vx * dt;
    ResolveX(level);
    y += vy * dt;
    onGround = false;
    ResolveY(level);
    if (x < 0) {
        x = 0;
        vx = 0;
    }
    CheckHazardsAndGoal(level, sounds);

    if (onGround && std::abs(vx) > 10) {
        _walkTimer += dt;
        if (_walkTimer > 0.15) {
            _walkTimer = 0;
            _walkFrame = 1 - _walkFrame;
        }
    } else {
        _walkFrame = 0;
        _walkTimer = 0;
    }
}

void Player::ResolveX(const Level& level) {
    const int cLeft = TileCoordinate(x);
    const int cRight = TileCoordinate(x + w - 1);
    const int rTop = TileCoordinate(y + 2);
    const int rBot = TileCoordinate(y + h - 2);
    for (int r = rTop; r <= rBot; ++r) {
        if (vx < 0 && level.IsSolid(cLeft, r)) {
            x = (cLeft + 1) * kTileSize;
            vx = 0;
            break;
        }
        if (vx > 0 && level.IsSolid(cRight, r)) {
            x = cRight * kTileSize - w;
            vx = 0;
            break;
        }
    }
}

void Player::ResolveY(const Level& level) {
    const int cLeft = TileCoordinate(x + 2);
    const int cRight = TileCoordinate(x + w - 2);
    const int rTop = TileCoordinate(y);
    const int rBot = TileCoordinate(y + h);
    if (vy < 0) {
        for (int c = cLeft; c <= cRight; ++c) {
            if (level.IsSolid(c, rTop)) {
                y = (rTop + 1) * kTileSize;
                vy = 0;
                break;
            }
        }
    } else {
        for (int c = cLeft; c <= cRight; ++c) {
            if (level.IsSolid(c, rBot)) {
                y = rBot * kTileSize - h;
                vy = 0;
                onGround = true;
                break;
            }
            if (level.IsPlatform(c, rBot)) {
                const double tileTop = rBot * kTileSize;
                if (y + h - vy * kFixedDt <= tileTop + 2) {
                    y = tileTop - h;
                    vy = 0;
                    onGround = true;
                    break;
                }
            }
        }
    }
}

void Player::CheckHazardsAndGoal(const Level& level, SoundEvents& sounds) {
    const int cLeft = TileCoordinate(x);
    const int cRight = TileCoordinate(x + w - 1);
    const int rTop = TileCoordinate(y);
    const int rBot = TileCoordinate(y + h - 1);
    for (int r = rTop; r <= rBot; ++r) {
        for (int c = cLeft; c <= cRight; ++c) {
            const int tile = level.TileAt(c, r);
            if (tile == 3 && _iframes <= 0) {
                Die(sounds);
                return;
            }
            if (tile == 4) {
                reachedGoal = true;
                return;
            }
        }
    }
    if (y > level.rows * kTileSize) Die(sounds);
}

void Player::Die(SoundEvents& sounds) {
    if (_iframes > 0) return;
    alive = false;
    sounds.push_back("die");
}

bool Player::Hurt(SoundEvents& sounds) {
    if (_iframes > 0) return false;
    _iframes = 1.5;
    prismaMode = false;
    vy = kJumpVelocity * 0.5;
    sounds.push_back("die");
    return true;
}

void Player::Respawn(double startX, double startY) {
    x = startX;
    y = startY;
    vx = 0;
    vy = 0;
    alive = true;
    reachedGoal = false;
    prismaMode = false;
    _prismaTimer = 0;
    _iframes = 1.0;
    _coyote = 0;
    _jumpBuf = 0;
    _doubleJumpUsed = false;
    // The original respawn retains onGround, walk animation and facing direction.
}

Enemy::Enemy(const EnemySpawn& spawn)
    : type(spawn.type == "jump" ? "jump" : "ground"),
      x(spawn.x * kTileSize), y(spawn.y * kTileSize), _speed(spawn.speed) {
    if (type == "jump") {
        w = 26;
        h = 24;
    } else {
        vx = spawn.speed;
    }
}

double Enemy::CenterX() const { return x + w / 2; }
double Enemy::CenterY() const { return y + h / 2; }
double Enemy::Right() const { return x + w; }
double Enemy::Bottom() const { return y + h; }

void Enemy::ResolveX(const Level& level) {
    const int rTop = TileCoordinate(y + 2);
    const int rBot = TileCoordinate(y + h - 2);
    if (vx < 0) {
        const int cLeft = TileCoordinate(x);
        for (int r = rTop; r <= rBot; ++r) {
            if (level.IsSolid(cLeft, r)) {
                x = (cLeft + 1) * kTileSize;
                vx = -vx;
                return;
            }
        }
    } else {
        const int cRight = TileCoordinate(x + w - 1);
        for (int r = rTop; r <= rBot; ++r) {
            if (level.IsSolid(cRight, r)) {
                x = cRight * kTileSize - w;
                vx = -vx;
                return;
            }
        }
    }
}

bool Enemy::ResolveY(const Level& level) {
    const int cLeft = TileCoordinate(x + 2);
    const int cRight = TileCoordinate(x + w - 2);
    if (vy > 0) {
        const int rBot = TileCoordinate(y + h);
        for (int c = cLeft; c <= cRight; ++c) {
            if (level.IsSolid(c, rBot) || level.IsPlatform(c, rBot)) {
                y = rBot * kTileSize - h;
                vy = 0;
                return true;
            }
        }
    } else if (vy < 0) {
        const int rTop = TileCoordinate(y);
        for (int c = cLeft; c <= cRight; ++c) {
            if (level.IsSolid(c, rTop)) {
                y = (rTop + 1) * kTileSize;
                vy = 0;
                break;
            }
        }
    }
    return false;
}

std::string Enemy::OverlapWithPlayer(const Player& player) const {
    const double px = player.x + 2;
    const double pr = player.Right() - 2;
    const double py = player.y;
    const double pb = player.Bottom();
    const double ex = x;
    const double er = Right();
    const double ey = y + 4;
    const double eb = Bottom();
    if (pr <= ex || px >= er || pb <= ey || py >= eb) return "none";
    const double topQuarter = ey + (eb - ey) * 0.4;
    if (player.vy > 0 && pb < topQuarter + 8) return "top";
    return "side";
}

void Enemy::Defeat(SoundEvents& sounds) {
    alive = false;
    sounds.push_back("stomp");
}

void Enemy::Update(double dt, const Level& level) {
    if (!alive) return;
    vy += kGravity * dt;
    if (vy > kMaxFallSpeed) vy = kMaxFallSpeed;
    if (type == "jump") {
        _jumpTimer += dt;
        if (_onGround && _jumpTimer >= _jumpInterval) {
            vy = _jumpForce;
            _onGround = false;
            _jumpTimer = 0;
        }
        y += vy * dt;
        _onGround = ResolveY(level);
        return;
    }

    x += vx * dt;
    ResolveX(level);
    const int nextCol = TileCoordinate(x + (vx > 0 ? w + 2 : -2));
    const int footRow = TileCoordinate(y + h + 2);
    if (!level.IsSolid(nextCol, footRow) && !level.IsPlatform(nextCol, footRow)) vx = -vx;
    y += vy * dt;
    ResolveY(level);
    _walkTimer += dt;
    if (_walkTimer > 0.2) {
        _walkTimer = 0;
        _walkFrame = 1 - _walkFrame;
    }
}

void Model::Load(const std::vector<nlohmann::json>& levels) {
    if (levels.empty()) throw std::runtime_error("Pixel Hop Twins needs at least one level");
    _levelData = levels;
    StartLevel(0);
}

void Model::StartLevel(int index) {
    _levelIndex = index;
    _level = Level(_levelData.at(static_cast<size_t>(index)));
    _camera = Camera(_level.cols, _level.rows);
    _timeLeft = _level.timeLimit;
    _player = Player(_level.playerStart.x * kTileSize, _level.playerStart.y * kTileSize);
    _camera.x = std::max(0.0, _player.CenterX() - kCanvasWidth / 2);
    _camera.y = std::max(0.0, _player.CenterY() - kCanvasHeight / 2);
    _enemies.clear();
    for (const auto& spawn : _level.enemies) _enemies.emplace_back(spawn);
    _state = "playing";
}

void Model::RespawnPlayer() {
    _player.Respawn(_level.playerStart.x * kTileSize, _level.playerStart.y * kTileSize);
    _enemies.clear();
    for (const auto& spawn : _level.enemies) _enemies.emplace_back(spawn);
    for (auto& gem : _level.gems) gem.collected = false;
    for (auto& powerUp : _level.powerUps) powerUp.collected = false;
    _state = "playing";
}

void Model::Update(double dt, const InputFrame& input) {
    soundEvents.clear();
    if (input.mute) muted = !muted;
    if (_state == "playing") UpdatePlaying(dt, input);
    else if (_state == "paused") UpdatePaused(input);
    else if (_state == "dead") UpdateDead(dt, input);
    else if (_state == "levelComplete") UpdateComplete(dt, input);
    else if (_state == "gameOver") UpdateGameOver(input);
}

void Model::UpdatePlaying(double dt, const InputFrame& input) {
    if (input.pause) {
        _state = "paused";
        return;
    }
    _timeLeft -= dt;
    if (_timeLeft <= 0) {
        _timeLeft = 0;
        LoseLife();
        return;
    }
    _level.Update(dt);
    _player.Update(dt, _level, input, soundEvents);
    for (auto& enemy : _enemies) enemy.Update(dt, _level);
    _camera.Update(_player.CenterX(), _player.CenterY(), dt);
    CheckEnemyCollisions();
    CheckGemCollection();
    CheckPowerUpCollection();
    if (!_player.alive) {
        // Preserve upstream ordering: side hits can consume a second life here.
        LoseLife();
        return;
    }
    if (_player.reachedGoal) {
        _state = "levelComplete";
        _completeTimer = 3;
        soundEvents.push_back("levelComplete");
    }
}

void Model::UpdatePaused(const InputFrame& input) {
    if (input.pause || input.restart) _state = "playing";
}

void Model::UpdateDead(double dt, const InputFrame& input) {
    _deathTimer -= dt;
    if (_deathTimer <= 0) {
        if (_lives > 0) RespawnPlayer();
        else _state = "gameOver";
    }
    if (input.restart) {
        if (_lives > 0) RespawnPlayer();
        else FullReset();
    }
}

void Model::UpdateComplete(double dt, const InputFrame& input) {
    _completeTimer -= dt;
    if (_completeTimer <= 0 || input.restart) {
        const int nextIndex = _levelIndex + 1;
        if (nextIndex < static_cast<int>(_levelData.size())) StartLevel(nextIndex);
        else {
            _state = "gameOver";
            _victory = true;
        }
    }
}

void Model::UpdateGameOver(const InputFrame& input) {
    if (input.restart) FullReset();
}

void Model::FullReset() {
    _lives = 3;
    _score = 0;
    _victory = false;
    StartLevel(0);
}

void Model::LoseLife() {
    _lives = std::max(0, _lives - 1);
    _player.alive = false;
    _state = "dead";
    _deathTimer = 2.0;
}

void Model::CheckEnemyCollisions() {
    for (auto& enemy : _enemies) {
        if (!enemy.alive) continue;
        const auto side = enemy.OverlapWithPlayer(_player);
        if (side == "top") {
            enemy.Defeat(soundEvents);
            _score += 200;
            _player.vy = -300;
        } else if (side == "side") {
            if (_player.Hurt(soundEvents)) {
                LoseLife();
                return;
            }
        }
    }
}

void Model::CheckGemCollection() {
    const double px = _player.x;
    const double pw = _player.w;
    const double py = _player.y;
    const double ph = _player.h;
    for (auto& gem : _level.gems) {
        if (gem.collected) continue;
        const double gx = gem.x * kTileSize + kTileSize * 0.15;
        const double gy = gem.y * kTileSize + kTileSize * 0.15;
        const double size = kTileSize * 0.7;
        if (px < gx + size && px + pw > gx && py < gy + size && py + ph > gy) {
            gem.collected = true;
            _score += 100;
            soundEvents.push_back("gem");
        }
    }
}

void Model::CheckPowerUpCollection() {
    const double px = _player.x;
    const double pw = _player.w;
    const double py = _player.y;
    const double ph = _player.h;
    for (auto& powerUp : _level.powerUps) {
        if (powerUp.collected) continue;
        const double gx = powerUp.x * kTileSize;
        const double gy = powerUp.y * kTileSize;
        if (px < gx + kTileSize && px + pw > gx && py < gy + kTileSize && py + ph > gy) {
            powerUp.collected = true;
            _player.ActivatePrisma(soundEvents);
        }
    }
}

} // namespace PixelHop
