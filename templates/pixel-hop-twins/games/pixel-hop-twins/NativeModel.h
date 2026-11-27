// Native translation of Pixel Hop Twins by stm1978, revision 9835295888c7.
// Original code is MIT licensed. See the licenses/MIT_LICENSE.txt and licenses/UPSTREAM.md.
#pragma once

#include "json.hpp"

#include <string>
#include <vector>

namespace PixelHop {

inline constexpr double kFixedDt = 1.0 / 60.0;
inline constexpr int kTileSize = 32;
inline constexpr int kCanvasWidth = 640;
inline constexpr int kCanvasHeight = 400;
using SoundEvents = std::vector<std::string>;

struct InputFrame {
    bool left = false;
    bool right = false;
    bool jumpHeld = false;
    bool jumpPressed = false;
    bool jumpReleased = false;
    bool pause = false;
    bool restart = false;
    bool mute = false;
};

struct Point {
    double x = 0;
    double y = 0;
};

struct Collectible : Point {
    bool collected = false;
};

struct EnemySpawn : Point {
    std::string type = "ground";
    double speed = 80;
};

struct Level {
    std::string name = "Level";
    double timeLimit = 200;
    std::vector<std::vector<int>> tiles;
    int rows = 0;
    int cols = 0;
    std::vector<Collectible> gems;
    std::vector<Collectible> powerUps;
    Point playerStart;
    std::vector<EnemySpawn> enemies;
    double _t = 0;

    Level() = default;
    explicit Level(const nlohmann::json& data);
    int TileAt(int col, int row) const;
    bool IsSolid(int col, int row) const;
    bool IsPlatform(int col, int row) const;
    bool IsSpike(int col, int row) const;
    bool IsFloor(int col, int row) const;
    void Update(double dt);
};

struct Camera {
    double x = 0;
    double y = 0;
    double _levelW = 0;
    double _levelH = 0;

    Camera() = default;
    Camera(int levelCols, int levelRows);
    void Update(double targetX, double targetY, double dt);
};

struct Player {
    double x = 0;
    double y = 0;
    double vx = 0;
    double vy = 0;
    double w = 23;
    double h = 35;
    bool onGround = false;
    double _coyote = 0;
    double _jumpBuf = 0;
    bool _doubleJumpUsed = false;
    bool prismaMode = false;
    double _prismaTimer = 0;
    bool alive = true;
    bool reachedGoal = false;
    double _walkTimer = 0;
    int _walkFrame = 0;
    bool _facingLeft = false;
    double _iframes = 0;

    Player() = default;
    Player(double startX, double startY);
    double CenterX() const;
    double CenterY() const;
    double Right() const;
    double Bottom() const;
    void ActivatePrisma(SoundEvents& sounds);
    void DeactivatePrisma();
    void Update(double dt, const Level& level, const InputFrame& input, SoundEvents& sounds);
    void ResolveX(const Level& level);
    void ResolveY(const Level& level);
    void CheckHazardsAndGoal(const Level& level, SoundEvents& sounds);
    void Die(SoundEvents& sounds);
    bool Hurt(SoundEvents& sounds);
    void Respawn(double startX, double startY);
};

struct Enemy {
    std::string type = "ground";
    double x = 0;
    double y = 0;
    double vx = 0;
    double vy = 0;
    double w = 29;
    double h = 26;
    bool alive = true;
    double _squishTimer = 0;
    double _speed = 80;
    double _walkTimer = 0;
    int _walkFrame = 0;
    bool _onGround = false;
    double _jumpTimer = 0;
    double _jumpInterval = 1.2;
    double _jumpForce = -480;

    Enemy() = default;
    explicit Enemy(const EnemySpawn& spawn);
    double CenterX() const;
    double CenterY() const;
    double Right() const;
    double Bottom() const;
    void ResolveX(const Level& level);
    bool ResolveY(const Level& level);
    std::string OverlapWithPlayer(const Player& player) const;
    void Defeat(SoundEvents& sounds);
    void Update(double dt, const Level& level);
};

struct Model {
    std::string _state = "loading";
    int _levelIndex = 0;
    int _lives = 3;
    int _score = 0;
    double _timeLeft = 200;
    std::vector<nlohmann::json> _levelData;
    Level _level;
    Player _player;
    std::vector<Enemy> _enemies;
    Camera _camera;
    double _deathTimer = 0;
    double _completeTimer = 0;
    bool _victory = false;
    bool muted = false;
    SoundEvents soundEvents;

    void Load(const std::vector<nlohmann::json>& levels);
    void StartLevel(int index);
    void RespawnPlayer();
    void Update(double dt, const InputFrame& input);
    void UpdatePlaying(double dt, const InputFrame& input);
    void UpdatePaused(const InputFrame& input);
    void UpdateDead(double dt, const InputFrame& input);
    void UpdateComplete(double dt, const InputFrame& input);
    void UpdateGameOver(const InputFrame& input);
    void FullReset();
    void LoseLife();
    void CheckEnemyCollisions();
    void CheckGemCollection();
    void CheckPowerUpCollection();
};

} // namespace PixelHop
