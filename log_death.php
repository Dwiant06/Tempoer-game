<?php
// tempoer/log_death.php — ROBOT ONLY; HMAC ON; baca jejak pemukul (critical 1.5x + stun 2s)
// - Damage default 20; pemukul valid → 30 (1.5x)
// - Non-lethal dari pemukul → stun 2s (extend minimal, UTC)
// - Lethal → freeze 20s (UTC)
// - Saat frozen: hanya pukul dari pemukul yang tetap mengurangi HP
// - LOG: Hit & Death (pakai match running bila ada)
// - Debug: ?debug=1, Verbose: ?verbose=1

header('Content-Type: application/json; charset=utf-8');

/* ===== Robust db_config ===== */
$__db_candidates = [
  __DIR__ . '/db_config.php',
  dirname(__DIR__) . '/db_config.php',
];
$__db_loaded = false;
foreach ($__db_candidates as $__p) { if (is_file($__p)) { require $__p; $__db_loaded = true; break; } }
if (!$__db_loaded) { echo json_encode(["status"=>"error","message"=>"db_config.php_not_found","tried_paths"=>$__db_candidates]); exit; }

/* ===== CONFIG ===== */
define('ATTACK_BUFFER_PATH', dirname(__DIR__) . '/action/skullbasher/attack_buffer.json');
const DEFAULT_DAMAGE        = 20;
const CRIT_MULTIPLIER       = 1.5;
const STUN_HIT_SECONDS      = 2;
const FREEZE_SECONDS        = 20;
const REQUIRE_HMAC_PLAYER   = true;
const LOG_EVERY_HIT         = true;
const VERBOSE_JSON_DEFAULT  = false;
const DEBUG_JSON_DEFAULT    = false;
const ATTACK_TTL_MS         = 2000;

/* ===== TZ ke UTC ===== */
@$mysqli->query("SET time_zone = '+00:00'");

/* ===== HELPERS ===== */
function is_turret($code){ return strpos(strtolower($code),'turret_') === 0; }
function is_player($code){ return preg_match('/^robot_[bo]\d+$/i',$code)===1; }
function json_err($msg,$extra=[]){ echo json_encode(array_merge(["status"=>"error","message"=>$msg],$extra)); exit; }
function team_from_code($code){
  $lc=strtolower($code); if(strpos($lc,'robot_b')===0) return 'blue'; if(strpos($lc,'robot_o')===0) return 'orange'; return null;
}

/* ===== INPUT (KORBAN) ===== */
$robot_code = strtolower(trim($_REQUEST['robot_code'] ?? ''));
$detail     = $_REQUEST['detail']   ?? 'Hit';
$damage_in  = isset($_REQUEST['damage']) ? (int)$_REQUEST['damage'] : DEFAULT_DAMAGE;
$baseDamage = max(0, min(100, $damage_in));
$sig        = strtolower(trim($_REQUEST['sig'] ?? ''));

// switches
$verbose = VERBOSE_JSON_DEFAULT;
if (isset($_GET['verbose'])) { $v=strtolower((string)$_GET['verbose']); $verbose = ($v==='1'||$v==='true'||$v==='yes'); }
$debug_on = DEBUG_JSON_DEFAULT;
if (isset($_GET['debug']))   { $d=strtolower((string)$_GET['debug']);   $debug_on = ($d==='1'||$d==='true'||$d==='yes'); }
$debug = [];

if ($robot_code === '' || !preg_match('/^[a-z0-9_]{1,32}$/',$robot_code)) json_err("robot_code_invalid");
if (is_turret($robot_code)) json_err("not_applicable_for_turret");
if (!is_player($robot_code)) json_err("not_a_player_robot");

/* ===== HMAC wajib utk player ===== */
if (REQUIRE_HMAC_PLAYER){
  if (!preg_match('/^[a-f0-9]{64}$/',$sig)) json_err("signature_invalid_or_missing");
  $stmt = $mysqli->prepare("SELECT TRIM(LOWER(secret_hex)) AS secret_hex, status FROM robot_auth WHERE robot_code=? LIMIT 1");
  if (!$stmt) json_err("db_prepare_failed", ["errno"=>$mysqli->errno,"error"=>$mysqli->error]);
  $stmt->bind_param("s",$robot_code); $stmt->execute();
  $auth = $stmt->get_result()->fetch_assoc(); $stmt->close();
  if (!$auth || $auth['status']!=='active') json_err("robot_not_registered_or_inactive");
  $secret_hex = $auth['secret_hex'];
  if (!ctype_xdigit($secret_hex) || strlen($secret_hex)!==64) json_err("secret_corrupt");
  $calc = hash_hmac('sha256', "death|{$robot_code}", pack('H*',$secret_hex));
  if (!hash_equals($calc,$sig)) json_err("signature_mismatch");
}

/* ===== Match running utk logging ===== */
$log_mid=null; $log_type=null;
$mq = @$mysqli->query("SELECT match_id, match_type FROM matches WHERE status='running' ORDER BY match_id DESC LIMIT 1");
if ($mq && $mq->num_rows){ $m=$mq->fetch_assoc(); $log_mid=(int)$m['match_id']; $log_type=$m['match_type']; }

/* ===== TRANSAKSI ===== */
try {
  $mysqli->begin_transaction();

  // Baseline row (idempoten)
  $ins = $mysqli->prepare("INSERT IGNORE INTO robot_status (robot_code, health, is_frozen, freeze_until, last_ping, role) VALUES (?,100,0,NULL,UTC_TIMESTAMP(),'player')");
  if ($ins){ $ins->bind_param("s",$robot_code); $ins->execute(); $ins->close(); }

  // Ambil status & lock
  $stmt = $mysqli->prepare("
    SELECT
      COALESCE(health,100) AS health,
      COALESCE(is_frozen,0) AS is_frozen,
      freeze_until,
      (freeze_until > UTC_TIMESTAMP()) AS freeze_active,
      GREATEST(0, TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), freeze_until)) AS remain_sec
    FROM robot_status
    WHERE robot_code=? FOR UPDATE
  ");
  if (!$stmt) throw new Exception("prepare_select_failed: ".$mysqli->error);
  $stmt->bind_param("s",$robot_code); $stmt->execute();
  $st = $stmt->get_result()->fetch_assoc();
  $stmt->close();
  if (!$st) throw new Exception("row_missing");

  $oldH          = (int)$st['health'];
  $is_frozen     = ((int)$st['is_frozen'] === 1);
  $freeze_active = ((int)$st['freeze_active'] === 1);
  $remain_sec    = (int)$st['remain_sec'];
  $victim_team   = team_from_code($robot_code); // infer dari prefix

  /* ===== BACA & KONSUMSI buffer pemukul ===== */
  $attacker = null; $att_age_ms = null; $from_striker = false;
  $debug['buffer_path'] = ATTACK_BUFFER_PATH;

  if (is_file(ATTACK_BUFFER_PATH) && is_readable(ATTACK_BUFFER_PATH)) {
    $fp = fopen(ATTACK_BUFFER_PATH, 'c+');
    if ($fp) {
      flock($fp, LOCK_EX); rewind($fp);
      $raw  = stream_get_contents($fp);
      $buf  = $raw ? json_decode($raw, true) : [];
      if (!is_array($buf)) $buf = [];
      $now_ms = (int)round(microtime(true)*1000);
      $debug['buffer_entries'] = count($buf);

      $pick_key = null; $pick_age = PHP_INT_MAX;
      foreach ($buf as $k => $rec) {
        $att_team = $rec['team'] ?? null;
        $mid      = isset($rec['match_id']) ? (int)$rec['match_id'] : null;
        $ts       = (int)($rec['ts_srv'] ?? 0);
        $age      = $now_ms - $ts;
        if ($age < 0 || $age > ATTACK_TTL_MS) continue;
        if ($victim_team && $att_team && $att_team === $victim_team) continue; // harus lawan
        if ($log_mid !== null && $mid !== null && $mid !== $log_mid) continue; // jika keduanya ada, harus sama
        if ($age < $pick_age){ $pick_key=$k; $pick_age=$age; }
      }

      if ($pick_key !== null) {
        $attacker     = $pick_key;
        $att_age_ms   = $pick_age;
        $from_striker = true;
        unset($buf[$pick_key]); // konsumsi
        // bersihkan expired
        foreach ($buf as $k => $rec) {
          $age = $now_ms - (int)($rec['ts_srv'] ?? 0);
          if ($age > ATTACK_TTL_MS) unset($buf[$k]);
        }
        ftruncate($fp, 0); rewind($fp);
        fwrite($fp, json_encode($buf, JSON_UNESCAPED_SLASHES));
      } else {
        $debug['picked_attacker'] = null;
        $debug['reason'] = 'no_recent_attack_or_same_team_or_diff_match';
      }
      flock($fp, LOCK_UN); fclose($fp);
    } else {
      $debug['buffer_open'] = 'fail';
    }
  } else {
    $debug['buffer_status'] = 'missing_or_unreadable';
  }

  /* ===== GATE frozen ===== */
  if ($is_frozen && $freeze_active && !$from_striker){
    $mysqli->commit();
    $resp = ["status"=>"frozen","health"=>$oldH];
    if ($verbose) $resp["meta"] = ["remaining"=>$remain_sec,"robot_code"=>$robot_code];
    if ($debug_on) $resp["debug"] = $debug;
    echo json_encode($resp); exit;
  }

  if ($oldH <= 0){
    $mysqli->commit();
    $resp = ["status"=>"already_dead","health"=>0];
    if ($verbose) $resp["meta"] = ["robot_code"=>$robot_code,"attacker"=>$attacker];
    if ($debug_on) $resp["debug"] = $debug;
    echo json_encode($resp); exit;
  }

  /* ===== Terapkan damage ===== */
  $damage = $from_striker ? (int)round(DEFAULT_DAMAGE * CRIT_MULTIPLIER) : $baseDamage;
  $newH   = max(0, $oldH - $damage);

  $u = $mysqli->prepare("UPDATE robot_status SET health=?, last_ping=UTC_TIMESTAMP() WHERE robot_code=?");
  if (!$u) throw new Exception("prepare_update_failed: ".$mysqli->error);
  $u->bind_param("is", $newH, $robot_code);
  $u->execute(); $u->close();

  $applied_freeze_str = null;

  // Non-lethal dari pemukul → stun 2s (extend minimal)
  if ($newH > 0 && $from_striker){
    $uS = $mysqli->prepare("
      UPDATE robot_status
      SET is_frozen=1,
          freeze_until = GREATEST(
            IFNULL(freeze_until, '1970-01-01 00:00:00'),
            DATE_ADD(UTC_TIMESTAMP(), INTERVAL ? SECOND)
          )
      WHERE robot_code=?
    ");
    if ($uS){
      $uS->bind_param("is", $stun = STUN_HIT_SECONDS, $robot_code);
      $uS->execute(); $uS->close();
      if ($verbose){
        $qfu = $mysqli->prepare("SELECT freeze_until FROM robot_status WHERE robot_code=? LIMIT 1");
        $qfu->bind_param("s",$robot_code); $qfu->execute(); $qfu->bind_result($applied_freeze_str); $qfu->fetch(); $qfu->close();
      }
    }
  }

  // Lethal → freeze 20s
  $freeze_until_str = null;
  if ($oldH > 0 && $newH === 0){
    $u2 = $mysqli->prepare("
      UPDATE robot_status
      SET is_frozen=1,
          freeze_until = DATE_ADD(UTC_TIMESTAMP(), INTERVAL ? SECOND)
      WHERE robot_code=?
    ");
    if ($u2){
      $u2->bind_param("is", $freezeSec = FREEZE_SECONDS, $robot_code);
      $u2->execute(); $u2->close();
      if ($verbose){
        $qfu = $mysqli->prepare("SELECT freeze_until FROM robot_status WHERE robot_code=? LIMIT 1");
        $qfu->bind_param("s",$robot_code); $qfu->execute(); $qfu->bind_result($freeze_until_str); $qfu->fetch(); $qfu->close();
      }
    }
  }

  $mysqli->commit();

  /* ===== LOG EVENT (post-commit) ===== */
  if ($log_mid !== null){
    // player_id & team_id opsional
    $team_id=null; $player_id=null;
    if ($ps = @$mysqli->prepare("SELECT player_id, team_id FROM players WHERE robot_code=? LIMIT 1")){
      $ps->bind_param("s",$robot_code); $ps->execute();
      if ($p=$ps->get_result()->fetch_assoc()){ $player_id=(int)$p['player_id']; $team_id=(int)$p['team_id']; }
      $ps->close();
    }

    if (LOG_EVERY_HIT){
      $ev = $mysqli->prepare("
        INSERT INTO game_event_log (`timestamp`, match_id, match_type, team_id, player_id, robot_code, event_type, detail)
        VALUES (UTC_TIMESTAMP(), ?, ?, ?, ?, ?, 'Hit', ?)
      ");
      if ($ev){
        $critStr = $from_striker ? "crit=1.5x; stun=2s; " : "";
        $det = $detail . "; " . $critStr . "damage={$damage}; hp:{$oldH}→{$newH}"
             . ($attacker ? "; attacker={$attacker}; age_ms={$att_age_ms}" : "");
        $ev->bind_param("isiiss", $log_mid, $log_type, $team_id, $player_id, $robot_code, $det);
        $ev->execute(); $ev->close();
      }
    }

    if ($oldH > 0 && $newH === 0){
      $ins = $mysqli->prepare("
        INSERT INTO game_event_log (`timestamp`, match_id, match_type, team_id, player_id, robot_code, event_type, detail)
        VALUES (UTC_TIMESTAMP(), ?, ?, ?, ?, ?, 'Death', ?)
      ");
      if ($ins){
        $detD = "via " . ($from_striker ? "pemukul" : "log_death") . "; freeze=" . FREEZE_SECONDS . "s"
              . ($attacker ? "; attacker={$attacker}; age_ms={$att_age_ms}" : "");
        $ins->bind_param("isiiss", $log_mid, $log_type, $team_id, $player_id, $robot_code, $detD);
        $ins->execute(); $ins->close();
      }
    }
  }

  /* ===== RESP ===== */
  $resp = [
    "status"        => "ok",
    "robot_code"    => $robot_code,
    "source"        => $from_striker ? "pemukul" : "default",
    "damage"        => $damage,
    "health"        => $newH,
    "is_dead"       => (int)($newH<=0),
    "freeze_until"  => $newH===0 ? ($freeze_until_str ?? null) : ($applied_freeze_str ?? null)
  ];
  if ($verbose){
    $resp["meta"] = [
      "old_health"    => $oldH,
      "new_health"    => $newH,
      "stun_on_hit"   => ($newH>0 && $from_striker) ? STUN_HIT_SECONDS : 0,
      "death_freeze"  => ($newH===0) ? FREEZE_SECONDS : 0,
      "attacker"      => $attacker,
      "att_age_ms"    => $att_age_ms,
    ];
  }
  if ($debug_on) $resp["debug"] = $debug;

  echo json_encode($resp);

} catch (Throwable $e){
  $mysqli->rollback();
  json_err("exception", ["error"=>$e->getMessage()]);
}
