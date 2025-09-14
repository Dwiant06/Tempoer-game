<?php
// tempoer/log_attack.php
// Simpan bukti serangan dari robot PEMUKUL ke buffer JSON (TTL 2 detik).
// TANPA HMAC. Buffer: ../action/skullbasher/attack_buffer.json
header('Content-Type: application/json; charset=utf-8');

/* ===== Robust load db_config.php (cek 2 lokasi umum) ===== */
$__db_candidates = [
  __DIR__ . '/db_config.php',           // kalau db_config.php ada di tempoer/
  dirname(__DIR__) . '/db_config.php',  // kalau ada di htdocs/ (parent)
];
$__db_loaded = false;
foreach ($__db_candidates as $__p) {
  if (is_file($__p)) { require $__p; $__db_loaded = true; break; }
}
if (!$__db_loaded) {
  echo json_encode([
    "status"=>"error",
    "message"=>"db_config.php_not_found",
    "tried_paths"=>$__db_candidates
  ]);
  exit;
}

/* ===== CONFIG ===== */
define('ATTACK_BUFFER_PATH', dirname(__DIR__) . '/action/skullbasher/attack_buffer.json');
const ATTACK_TTL_MS = 2000;
const DEBUG_DEF     = false;

/* ===== HELPERS ===== */
function jerr($msg, $extra = []) {
  echo json_encode(array_merge(["status"=>"error","message"=>$msg], $extra)); exit;
}
function infer_team_from_code(string $code): ?string {
  $lc = strtolower($code);
  if (strpos($lc, 'robot_b') === 0) return 'blue';
  if (strpos($lc, 'robot_o') === 0) return 'orange';
  return null;
}

/* ===== INPUT (PEMUKUL) ===== */
$robot_code = strtolower(trim($_REQUEST['robot_code'] ?? ''));
$ts_dev     = isset($_REQUEST['ts']) ? (int)$_REQUEST['ts'] : 0;
$g          = isset($_REQUEST['g'])  ? (float)$_REQUEST['g']  : 0.0;
$crit       = isset($_REQUEST['crit']) ? (int)$_REQUEST['crit'] : 0;

$debug_on = DEBUG_DEF;
if (isset($_GET['debug'])) {
  $dv = strtolower((string)$_GET['debug']);
  $debug_on = ($dv==='1' || $dv==='true' || $dv==='yes');
}
$dbg = [];

if ($robot_code === '' || !preg_match('/^[a-z0-9_]{1,32}$/', $robot_code)) jerr("robot_code_invalid");

/* ===== team & match_id ===== */
$team = infer_team_from_code($robot_code);
if ($team === null) jerr("cannot_infer_team_from_robot_code", ["robot_code"=>$robot_code]);

$match_id = null;
$mq = @$mysqli->query("SELECT match_id FROM matches WHERE status='running' ORDER BY match_id DESC LIMIT 1");
if ($mq && $mq->num_rows) {
  $m = $mq->fetch_assoc();
  $match_id = (int)$m['match_id'];
}

/* ===== Siapkan folder & file buffer ===== */
$dir = dirname(ATTACK_BUFFER_PATH);
$dbg['buffer_path'] = ATTACK_BUFFER_PATH;
$dbg['dir']         = $dir;

if (!is_dir($dir)) {
  @mkdir($dir, 0775, true);
  if (!is_dir($dir)) jerr("make_dir_failed", ["dir"=>$dir, "debug"=>$debug_on ? $dbg : null]);
  @chmod($dir, 0775);
}
$dbg['dir_is_writable'] = is_writable($dir) ? 'yes' : 'no';

if (!is_file(ATTACK_BUFFER_PATH)) {
  if (@file_put_contents(ATTACK_BUFFER_PATH, "{}") === false) {
    jerr("create_buffer_failed", ["path"=>ATTACK_BUFFER_PATH, "debug"=>$debug_on ? $dbg : null]);
  }
  @chmod(ATTACK_BUFFER_PATH, 0664);
}

/* ===== Buka + lock ===== */
$fp = @fopen(ATTACK_BUFFER_PATH, 'c+');
if (!$fp) jerr("open_buffer_failed", ["path"=>ATTACK_BUFFER_PATH, "debug"=>$debug_on ? $dbg : null]);

flock($fp, LOCK_EX);
rewind($fp);
$raw  = stream_get_contents($fp);
$data = $raw ? json_decode($raw, true) : [];
if (!is_array($data)) $data = [];

$now_ms = (int)round(microtime(true) * 1000);

/* ===== Autoclear kedaluwarsa ===== */
$cleared = 0;
foreach ($data as $k => $v) {
  $age = $now_ms - (int)($v['ts_srv'] ?? 0);
  if ($age < 0 || $age > ATTACK_TTL_MS) { unset($data[$k]); $cleared++; }
}

/* ===== Simpan/overwrite entri PEMUKUL ===== */
$data[$robot_code] = [
  'match_id' => $match_id,  // bisa null kalau tidak ada match running; tetap oke
  'team'     => $team,
  'ts_dev'   => $ts_dev,
  'ts_srv'   => $now_ms,
  'g'        => $g,
  'crit'     => $crit ? 1 : 0
];

/* ===== Tulis balik ===== */
ftruncate($fp, 0);
rewind($fp);
$ok = fwrite($fp, json_encode($data, JSON_UNESCAPED_SLASHES));
flock($fp, LOCK_UN);
fclose($fp);
if ($ok === false) jerr("write_buffer_failed", ["path"=>ATTACK_BUFFER_PATH]);

/* ===== Response ===== */
$out = [
  "status"   => "ok",
  "attacker" => $robot_code,
  "team"     => $team,
  "match_id" => $match_id,  // bisa null
  "ttl_ms"   => ATTACK_TTL_MS,
  "crit"     => $crit ? 1 : 0
];
if ($debug_on) {
  $out["debug"] = [
    "file_exists"      => is_file(ATTACK_BUFFER_PATH) ? 'yes' : 'no',
    "file_is_writable" => is_writable(ATTACK_BUFFER_PATH) ? 'yes' : 'no',
    "entries_after"    => count($data),
    "cleared"          => $cleared,
    "ts_srv"           => $now_ms
  ] + $dbg;
}
echo json_encode($out);
