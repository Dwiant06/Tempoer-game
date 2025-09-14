<?php
// robot_status.php — heartbeat/status (turret bypass HMAC); auto-clear stun & respawn (UTC-consistent)
header('Content-Type: text/plain; charset=utf-8');
require __DIR__.'/db_config.php';

/** SERAGAMKAN TZ KE UTC AGAR MATCH DGN freeze_from_lock.php */
@$mysqli->query("SET time_zone = '+00:00'");

/** KONFIG **/
const REQUIRE_HMAC = false;               // set true bila ingin aktifkan HMAC utk PLAYER
$ALLOWED_IPS = ['192.168.1.0/24'];        // kosongkan [] untuk menonaktifkan filter IP

/** Utils IP **/
function ip_in_cidr($ip, $cidr){
  if (strpos($cidr,'/')===false) return $ip===$cidr;
  list($subnet,$mask)=explode('/',$cidr,2);
  return (ip2long($ip) & ~((1<<(32-$mask))-1)) === (ip2long($subnet) & ~((1<<(32-$mask))-1));
}
function allowed_ip(array $cidrs){ if(!$cidrs) return true; $ip=$_SERVER['REMOTE_ADDR']??''; foreach($cidrs as $c){ if(ip_in_cidr($ip,$c)) return true; } return false; }

/** Input **/
$robot_code = strtolower(trim($_GET['robot_code'] ?? ''));
$sig        = strtolower($_GET['sig'] ?? '');

/** Gate dasar **/
if (!allowed_ip($ALLOWED_IPS))                          { echo "INACTIVE"; exit; }
if (!preg_match('/^[a-z0-9_]{1,32}$/',$robot_code))     { echo "INACTIVE"; exit; }

$isTurret = (strpos($robot_code,'turret_') === 0);
$isPlayer = (preg_match('/^robot_[bo]\d+$/', $robot_code) === 1);

/** Match harus RUNNING **/
$match = $mysqli->query("SELECT match_id, match_type FROM matches WHERE status='running' ORDER BY match_id DESC LIMIT 1")->fetch_assoc();
if (!$match){ echo "INACTIVE"; exit; }
$match_id   = (int)$match['match_id'];
$match_type = strtolower($match['match_type'] ?? '');

/** Auth / HMAC
 *  - Turret: BYPASS (tidak cek robot_auth & sig)
 *  - Player: cek hanya jika REQUIRE_HMAC=true
 */
if (!$isTurret && REQUIRE_HMAC) {
  $stmt = $mysqli->prepare("SELECT TRIM(LOWER(secret_hex)) AS secret_hex, status FROM robot_auth WHERE robot_code=? LIMIT 1");
  $stmt->bind_param("s",$robot_code); $stmt->execute();
  $auth = $stmt->get_result()->fetch_assoc(); $stmt->close();
  if(!$auth || $auth['status']!=='active') { echo "INACTIVE"; exit; }

  if (!preg_match('/^[a-f0-9]{64}$/',$sig)) { echo "INACTIVE"; exit; }
  $calc = hash_hmac('sha256','status',pack('H*',$auth['secret_hex']));
  if (!hash_equals($calc,$sig)) { echo "INACTIVE"; exit; }
}

/** Cek struktur robot_status (ada match_id atau tidak) **/
$cols = [];
if ($c=$mysqli->query("SHOW COLUMNS FROM `robot_status`")) { while($r=$c->fetch_assoc()) $cols[$r['Field']]=true; }
$hasMatchId = isset($cols['match_id']);

/** Upsert last_ping + role (+ match_id jika ada) — tidak menyentuh is_frozen/health */
$role = $isTurret ? 'turret' : 'player';
if ($hasMatchId) {
  $stmt=$mysqli->prepare("
    INSERT INTO robot_status (robot_code, role, is_frozen, freeze_until, last_ping, match_id)
    VALUES (?, ?, 0, NULL, UTC_TIMESTAMP(), ?)
    ON DUPLICATE KEY UPDATE last_ping=UTC_TIMESTAMP(), role=VALUES(role), match_id=VALUES(match_id)
  ");
  $stmt->bind_param("ssi",$robot_code,$role,$match_id);
} else {
  $stmt=$mysqli->prepare("
    INSERT INTO robot_status (robot_code, role, is_frozen, freeze_until, last_ping)
    VALUES (?, ?, 0, NULL, UTC_TIMESTAMP())
    ON DUPLICATE KEY UPDATE last_ping=UTC_TIMESTAMP(), role=VALUES(role)
  ");
  $stmt->bind_param("ss",$robot_code,$role);
}
$stmt->execute(); $stmt->close();

/** AUTO-CLEAR stun/respawn (KHUSUS PLAYER), pakai jam DB (UTC) */
if ($isPlayer) {
  // 1) kalau stun habis & HP>0 → lepas bendera (HP tetap)
  $u1 = $mysqli->prepare("
    UPDATE robot_status
    SET is_frozen = 0,
        freeze_until = NULL
    WHERE robot_code = ?
      AND role = 'player'
      AND is_frozen = 1
      AND COALESCE(health,100) > 0
      AND freeze_until IS NOT NULL
      AND freeze_until <= UTC_TIMESTAMP()
  ");
  $u1->bind_param("s", $robot_code);
  $u1->execute();
  $u1->close();

  // 2) kalau death freeze habis (HP=0) → respawn 100 + lepas bendera
  $u2 = $mysqli->prepare("
    UPDATE robot_status
    SET is_frozen = 0,
        freeze_until = NULL,
        health = 100
    WHERE robot_code = ?
      AND role = 'player'
      AND is_frozen = 1
      AND COALESCE(health,100) = 0
      AND freeze_until IS NOT NULL
      AND freeze_until <= UTC_TIMESTAMP()
  ");
  $u2->bind_param("s", $robot_code);
  $u2->execute();
  $u2->close();
}

/** Baca status terbaru (indikator freeze dihitung oleh DB) */
$stmt = $mysqli->prepare("
  SELECT
    COALESCE(health,100) AS health,
    COALESCE(is_frozen,0) AS is_frozen,
    (freeze_until > UTC_TIMESTAMP()) AS freeze_active,
    freeze_until
  FROM robot_status
  WHERE robot_code=? LIMIT 1
");
$stmt->bind_param("s", $robot_code);
$stmt->execute();
$row = $stmt->get_result()->fetch_assoc();
$stmt->close();

if (!$row) { echo "INACTIVE"; exit; }

$health        = (int)$row['health'];
$is_frozen_col = ((int)$row['is_frozen'] === 1);
$freeze_active = ((int)$row['freeze_active'] === 1);

/* ---------- TURRET ---------- */
/* Turret tidak punya stun 8s: kalau HP<=0 atau dibekukan manual (is_frozen=1) → INACTIVE */
if ($isTurret) {
  if ($health <= 0)    { echo "INACTIVE"; exit; }
  if ($is_frozen_col)  { echo "INACTIVE"; exit; }
  echo "ACTIVE"; exit;
}

/* ---------- PLAYER ---------- */
/* Jika DB bilang masih frozen (freeze_until di masa depan) → INACTIVE */
if ($freeze_active) { echo "INACTIVE"; exit; }

/* Selain itu aktif */
echo "ACTIVE"; exit;
