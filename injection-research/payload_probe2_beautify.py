# -*- coding: utf-8 -*-
# IDV 进程内载荷 = probe2(只读 + 文件命令通道) + 本地美化(换皮, UDP 55556 控制)
# 2026-10-17 合并。前半段 probe2 原样；后半段是若辰式 UDP 控制循环 + 今天验证过的 EXT=False 美化。
# 叠加层「本地美化」按钮 → UDP 127.0.0.1:55556 {"type":"hook_control","local_beautify_run":bool}
import sys, os, time, json, threading, traceback

D = "/sdcard/Android/data/com.netease.dwrg/files"
SNAP = D + "/idv_snap.json"
TL   = D + "/idv_timeline.jsonl"
ERR  = D + "/idv_err.txt"
CMD  = D + "/idv_cmd.py"        # 往这里写 Python，探针会执行
COUT = D + "/idv_cmd_out.txt"   # 执行结果

def _w(p, t, m="w"):
    try:
        f = open(p, m); f.write(t); f.close(); return True
    except Exception:
        return False

def _err(w):
    _w(ERR, "[%.1f] %s\n%s\n" % (time.time(), w, traceback.format_exc()), "a")

def S(fn, d=None):
    try:
        return fn()
    except Exception:
        return d

def R(v, n=240):
    try:
        s = repr(v)
    except Exception as e:
        s = "<repr fail %s>" % e.__class__.__name__
    return s if len(s) <= n else s[:n] + "...(%d)" % len(s)

def cname(o):
    return S(lambda: type(o).__name__, "?")

def idict(o):
    """只读实例 __dict__，不触发 property"""
    try:
        return object.__getattribute__(o, "__dict__")
    except Exception:
        return {}

def attr_names(o):
    return sorted([k for k in idict(o).keys() if isinstance(k, str)])

def nums(o):
    out = {}
    for k, v in list(idict(o).items()):
        if isinstance(k, str) and isinstance(v, (int, float)) and not isinstance(v, bool):
            out[k] = float(v)
    return out

def GK():
    import game_kernel as gk
    return gk

def get_mgr():
    gk = S(GK)
    if gk is None:
        return None
    m = S(lambda: getattr(gk, "unit_mgr", None))
    if m:
        return m
    p = S(lambda: getattr(gk, "g_unit", None))
    r = S(lambda: getattr(p, "room", None)) if p else None
    return S(lambda: getattr(r, "unit_mgr", None)) if r else None

def ubt(mgr):
    return S(lambda: getattr(mgr, "units_by_type", None)) or {}

def units_of(mgr, t):
    l = S(lambda: ubt(mgr).get(t))
    return S(lambda: list(l), []) if l is not None else []

def all_units(mgr):
    out = []
    t = ubt(mgr)
    for k in S(lambda: sorted([x for x in t.keys() if isinstance(x, int)]), []):
        for u in units_of(mgr, k):
            out.append((k, u))
    return out

# ---------- 命令通道 ----------
def run_cmd():
    src = S(lambda: open(CMD).read(), "")
    if not src or not src.strip():
        return
    try:
        os.remove(CMD)
    except Exception:
        _w(CMD, "")
    buf = []
    def P(*a):
        buf.append(" ".join(str(x) for x in a))
    mgr = get_mgr()
    g = {"__builtins__": __builtins__, "P": P, "OUT": buf,
         "GK": S(GK), "mgr": mgr, "S": S, "R": R, "json": json, "time": time,
         "idict": idict, "attr_names": attr_names, "nums": nums, "cname": cname,
         "units_of": (lambda t: units_of(mgr, t)), "all_units": (lambda: all_units(mgr)),
         "ubt": (lambda: ubt(mgr)),
         "me": S(lambda: getattr(S(GK), "g_unit", None))}
    t0 = time.time()
    try:
        exec(src, g)
    except Exception:
        buf.append(traceback.format_exc())
    _w(COUT, "# %.2f ms\n" % ((time.time() - t0) * 1000) + "\n".join(str(x) for x in buf))

# ---------- 快照 ----------
GKEY = "genius_id_lv_lst"
FLAG_HINTS = ("used", "ability", "trigger", "consume", "psv", "rescue",
              "save", "borrow", "revive", "dying", "hp")

def flags_of(u):
    o = {}
    d = idict(u)
    for k in sorted(d.keys()):
        if not isinstance(k, str):
            continue
        lk = k.lower()
        if any(h in lk for h in FLAG_HINTS):
            o[k] = R(d.get(k), 110)
    return o

def probe_unit(u):
    r = {"cls": cname(u), "uid": R(S(lambda: getattr(u, "uid", None)), 30)}
    g = S(lambda: getattr(u, GKEY, "<missing>"))
    r["g_len"] = S(lambda: len(g), -1)
    r["g"] = R(g, 300)
    r["g_in_dict"] = GKEY in idict(u)
    r["support"] = R(S(lambda: getattr(u, "support_skill_id", None)), 40)
    r["flags"] = flags_of(u)
    return r

_prev = {}
def diff(key, o, now):
    cur = nums(o); out = []
    old = _prev.get(key)
    if old:
        t0, p = old
        dt = now - t0
        if dt > 0.3:
            for k, v in cur.items():
                if k in p and v != p[k]:
                    out.append({"a": k, "p": p[k], "n": v, "r": round((v - p[k]) / dt, 3)})
                elif k not in p:
                    out.append({"a": k, "p": None, "n": v, "r": 0})
            for k in p:
                if k not in cur:
                    out.append({"a": k, "p": p[k], "n": None, "r": 0})
    _prev[key] = (now, cur)
    return out[:60]

_n = 0
_seen_types = set()

def snapshot():
    global _n
    _n += 1
    now = time.time()
    o = {"i": _n, "t": round(now, 2)}
    mgr = get_mgr()
    if mgr is None:
        o["mgr"] = None
        return o
    o["mgr"] = R(mgr, 70)
    t = ubt(mgr)
    keys = S(lambda: sorted([x for x in t.keys() if isinstance(x, int)]), [])
    o["types"] = {}
    for k in keys:
        us = units_of(mgr, k)
        c = {}
        for u in us:
            n = cname(u)
            c[n] = c.get(n, 0) + 1
        o["types"][str(k)] = {"n": len(us), "c": c}
        sig = (k, tuple(sorted(c.keys())))
        if sig not in _seen_types:      # 只在首次出现时 dump 属性名，控制体积
            _seen_types.add(sig)
            o["types"][str(k)]["attrs"] = attr_names(us[0])[:500] if us else []
    gk = S(GK)
    me = S(lambda: getattr(gk, "g_unit", None))
    o["me"] = {"cls": cname(me), "uid": R(S(lambda: getattr(me, "uid", None)), 30)}
    players = units_of(mgr, 1) + units_of(mgr, 2)
    if me is not None and all(me is not p for p in players):
        players.append(me)
    o["units"] = [probe_unit(u) for u in players[:12]]
    o["ud"] = {}
    for u in players[:12]:
        uid = R(S(lambda u=u: getattr(u, "uid", None)), 30)
        d = diff("u:" + uid, u, now)
        if d:
            o["ud"][uid] = d
    h = units_of(mgr, 1)
    if h:
        sm = S(lambda: getattr(h[0], "skill_mgr", None))
        if sm is not None:
            o["sm"] = {"btn": R(S(lambda: getattr(sm, "skill_btn_status", None)), 150),
                       "last": R(S(lambda: getattr(sm, "last_cast_skill_id", None)), 20),
                       "d": diff("sm", sm, now)}
            sd = S(lambda: getattr(sm, "skill_dict", None)) or {}
            sk = {}
            for sid, s in S(lambda: list(sd.items())[:30], []):
                e = {"cls": cname(s)}
                for k in ("_cd_delta", "cd_time", "csv_cd_time", "game_start_cd"):
                    e[k] = S(lambda k=k, s=s: getattr(s, k, None))
                dd = diff("s:%s" % sid, s, now)
                if dd:
                    e["d"] = dd
                sk[str(sid)] = e
            o["sk"] = sk
    return o

def loop():
    time.sleep(2.0)
    while True:
        try:
            run_cmd()
        except Exception:
            _err("cmd")
        try:
            s = snapshot()
            txt = json.dumps(s, ensure_ascii=False, default=str)
            _w(SNAP, txt); _w(TL, txt + "\n", "a")
        except Exception:
            _err("snap")
        time.sleep(2.0)

# ============================================================================
# 本地美化(换皮) —— 对齐若辰 kBeautifyScript.py，EXT=False(扩展资源会崩，见 本地美化\SKILL.md)
# 叠加层「本地美化」按钮经 UDP 55556 下发 {"type":"hook_control","local_beautify_run":bool}
# ============================================================================
BEAUTIFY_EXT = False     # ‼ 扩展资源(头像框/特效/动作)在后台线程连调会让引擎崩，默认关
_BF_DIAG = D + "/beautify_diag.json"
_BF_STATUS = D + "/beautify_status.txt"   # 心跳状态文件(叠加层 root 侧按新鲜度+字段读它)
_BF = {"installed": False, "requested": False, "last_error": "", "rows": 0}
_BF_LOCK = threading.Lock()
_BF_TAGS = (("LocalArticleUnlockHooked", "LocalArticleUnlockOriginal"),
            ("LocalResourceUpdateHooked", "LocalResourceUpdateOriginal"))

def _bf_diag(stage, extra=None):
    rep = {"stage": stage, "time": time.time(),
           "installed": _BF["installed"], "requested": _BF["requested"],
           "avatar": S(lambda: getattr(S(GK), "g_avatar", None)) is not None,
           "rows": _BF["rows"], "last_error": _BF["last_error"]}
    if extra:
        rep.update(extra)
    _w(_BF_DIAG, json.dumps(rep, ensure_ascii=False, sort_keys=True))

_BF_REPORT_PORT = 55555     # 回报端口(叠加层 bind 收)。55556 是控制入口(载荷 bind 收)。
_bf_report_sock = None

def _bf_write_status():
    """心跳：一行一个 key=val，纯文本好让叠加层无依赖解析。每 ~1s + 状态变化时发。
    ‼ 必须走 UDP：叠加层是独立 root 进程，跨 FUSE 读不到游戏包私有目录下的文件，
       所以文件只留作 adb 侧诊断，真正的回读走 UDP 127.0.0.1:55555(环回稳、与权限无关)。"""
    global _bf_report_sock
    av = S(lambda: getattr(S(GK), "g_avatar", None)) is not None
    lines = (
        "t=%.1f" % time.time(),          # 载荷墙钟时间(排查用)
        "avatar=%d" % (1 if av else 0),  # g_avatar 是否就绪(进大厅后才 True)
        "installed=%d" % (1 if _BF["installed"] else 0),
        "requested=%d" % (1 if _BF["requested"] else 0),
        "rows=%d" % int(_BF["rows"]),
        "error=%s" % ((_BF["last_error"] or "").replace("\n", " ")[:180]),
    )
    txt = "\n".join(lines) + "\n"
    _w(_BF_STATUS, txt)                   # 文件：仅供 adb 侧 cat 诊断
    try:                                  # UDP：叠加层真正读的通道
        import socket
        if _bf_report_sock is None:
            _bf_report_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        _bf_report_sock.sendto(txt.encode("utf-8"), ("127.0.0.1", _BF_REPORT_PORT))
    except Exception as e:
        _err("bf_report " + repr(e))

def _bf_unwrap(owner, name):
    fn = getattr(owner, name); n = 0
    while True:
        d = getattr(fn, "__dict__", {})
        for tag, okey in _BF_TAGS:
            if d.get(tag) == 1:
                fn = d[okey]; n += 1; break
        else:
            return fn, n

def _bf_copy(v):
    if isinstance(v, dict): return {k: _bf_copy(x) for k, x in v.items()}
    if isinstance(v, list): return [_bf_copy(x) for x in v]
    if isinstance(v, tuple): return tuple(_bf_copy(x) for x in v)
    return v

def beautify_enable():
    gk = S(GK)
    a = S(lambda: gk.g_avatar); dm = S(lambda: gk.DM)
    if a is None or dm is None:
        return "pending"
    from common_cs import ComuKeys
    _CLOTH = ComuKeys.ARTICLE_TYPE.CLOTH
    _DECO = ComuKeys.ARTICLE_TYPE.DECORATION
    AC = type(a)
    # 1. 追加拥有
    pending_c = []
    for cid, cdv in list(dm.cloth_data.items()):
        role = str(S(lambda cdv=cdv: cdv['role_id']))
        if role in a.role_cloth and cid not in a.role_cloth[role][1]:
            pending_c.append((role, cid))
    dl = a.article_unlock['decoration_list']
    pending_d = [did for did in list(dm.decoration_data) if did not in dl]
    for role, cid in pending_c:
        a.role_cloth[role][1].append(cid)
    for role in a.role_cloth:
        try: a.role_cloth[role][1].sort()
        except Exception: pass
    for did in pending_d:
        dl.append(did)
    a.update_unlock_cloth_type_info()
    # 2. 换皮 hook
    def _apply_sel(av, role_key, cloth_id):
        role_key = str(role_key)
        orig = getattr(av, 'LocalArticleUnlockOriginalCloths', None)
        if orig is None:
            orig = {}; av.LocalArticleUnlockOriginalCloths = orig
        if role_key not in orig and role_key in av.role_cloth:
            orig[role_key] = av.role_cloth[role_key][0]
        sel = getattr(av, 'LocalArticleUnlockSelectedCloths', None)
        if sel is None:
            sel = {}; av.LocalArticleUnlockSelectedCloths = sel
        sel[role_key] = cloth_id
        av.reply_update_cloth(cloth_id, {'role_id': int(role_key)})
        for ui_name in ('UIGoldRushArticle', 'UIBattleGoldRushChangeCloth'):
            try:
                ui = gk.g_ui_mgr.get_ui(ui_name)
                if ui: ui.update_item_on_setting(_CLOTH, cloth_id, int(role_key))
            except Exception: pass
    _orig_set, _ = _bf_unwrap(AC, 'set_cur_role_cloth')
    def _set_cloth(self, role_id_str, cloth_id, will_start_game, need_reply=False, match_type=None):
        _apply_sel(self, role_id_str, cloth_id); return None
    _set_cloth.LocalArticleUnlockHooked = 1
    _set_cloth.LocalArticleUnlockOriginal = _orig_set
    AC.set_cur_role_cloth = _set_cloth
    sel = getattr(a, 'LocalArticleUnlockSelectedCloths', None)
    if sel is None:
        sel = {}; a.LocalArticleUnlockSelectedCloths = sel
    for role in list(getattr(a, 'LocalArticleUnlockOriginalCloths', {}) or {}):
        if role not in sel and role in a.role_cloth:
            sel[role] = a.role_cloth[role][0]
    # 3. 挂件 hook
    _orig_ua, _ = _bf_unwrap(AC, 'update_article')
    def _ua(self, article_type, article_id, info=None):
        if article_type == _DECO:
            self.reply_update_decoration(article_id, info); return None
        return _orig_ua(self, article_type, article_id, info)
    _ua.LocalArticleUnlockHooked = 1
    _ua.LocalArticleUnlockOriginal = _orig_ua
    AC.update_article = _ua
    # 4. 局内套皮 hook
    def _local_cloth(av, role_id):
        rk = str(role_id)
        if rk not in av.role_cloth: return None
        return (getattr(av, 'LocalArticleUnlockSelectedCloths', None) or {}).get(rk, av.role_cloth[rk][0])
    def _build_local(params, av, role_id):
        local = dict(params)
        if isinstance(getattr(av, 'article_setting', None), dict):
            local['article_setting'] = _bf_copy(av.article_setting)
        cid = _local_cloth(av, role_id)
        if cid is not None and cid in dm.cloth_data:
            local['cloth_id'] = cid
            mp = S(lambda: dm.cloth_data[cid]['model_path'])
            if mp: local['model'] = mp
        return local
    try:
        from ClientUnits.BaseUnit import BaseUnit
        _orig_init, _ = _bf_unwrap(BaseUnit, '__init__')
        def _unit_init(self, params, *args, **kw):
            try:
                if isinstance(params, dict):
                    ri = getattr(params.get('room'), 'info_dict', None)
                    su = ri.get('self_uid') if hasattr(ri, 'get') else None
                    av = gk.g_avatar
                    if su is not None and av is not None and params.get('uid') == su:
                        local = _build_local(params, av, params.get('pid'))
                        params.clear(); params.update(local)
            except Exception:
                pass
            return _orig_init(self, params, *args, **kw)
        _unit_init.LocalArticleUnlockHooked = 1
        _unit_init.LocalArticleUnlockOriginal = _orig_init
        BaseUnit.__init__ = _unit_init
    except Exception as e:
        _BF["last_error"] = "unit_hook:" + repr(e)
    _BF["rows"] = len(pending_c) + len(pending_d)
    return "ok"

def beautify_disable():
    gk = S(GK); a = S(lambda: gk.g_avatar)
    if a is None:
        return "pending"
    AC = type(a)
    for mname in ("set_cur_role_cloth", "update_article"):
        fn, n = _bf_unwrap(AC, mname)
        if n: setattr(AC, mname, fn)
    try:
        from ClientUnits.BaseUnit import BaseUnit
        fn, n = _bf_unwrap(BaseUnit, '__init__')
        if n: BaseUnit.__init__ = fn
    except Exception: pass
    orig = getattr(a, 'LocalArticleUnlockOriginalCloths', None)
    if isinstance(orig, dict):
        for role, cloth in list(orig.items()):
            try: a.reply_update_cloth(cloth, {'role_id': int(role)})
            except Exception: pass
    for attr in ('LocalArticleUnlockOriginalCloths', 'LocalArticleUnlockSelectedCloths'):
        try: delattr(a, attr)
        except Exception: pass
    try: a.update_unlock_cloth_type_info()
    except Exception: pass
    return "ok"

def _bf_sync():
    want = _BF["requested"]
    if want and not _BF["installed"]:
        try:
            r = beautify_enable()
            if r == "ok":
                _BF["installed"] = True; _BF["last_error"] = ""
                _bf_diag("enabled", {"rows": _BF["rows"]})
            elif r == "pending":
                _bf_diag("enable_pending")
        except Exception as e:
            _BF["last_error"] = repr(e); _bf_diag("enable_error", {"error": repr(e)})
    elif (not want) and _BF["installed"]:
        try:
            r = beautify_disable()
            if r == "ok":
                _BF["installed"] = False; _BF["last_error"] = ""
                _bf_diag("disabled")
        except Exception as e:
            _BF["last_error"] = repr(e); _bf_diag("disable_error", {"error": repr(e)})

def beautify_control_loop():
    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(('127.0.0.1', 55556)); sock.setblocking(False)
    except Exception as e:
        _err("bf_bind " + repr(e)); return
    _bf_diag("control_thread_started")
    _bf_write_status()                  # 一注入就落地心跳文件，叠加层立刻能看到"载荷在线"
    last_retry = 0.0
    last_status = 0.0
    while True:
        try:
            while True:
                try:
                    pkt = sock.recv(4096)
                except BlockingIOError:
                    break
                try:
                    data = json.loads(pkt.decode('utf-8'))
                except Exception:
                    data = None
                if isinstance(data, dict) and data.get('type') == 'hook_control' and 'local_beautify_run' in data:
                    with _BF_LOCK:
                        _BF["requested"] = bool(data['local_beautify_run'])
                    _bf_sync()
                    _bf_write_status()          # 收到开关后立即回报，不等心跳
            now = time.time()
            if _BF["requested"] and not _BF["installed"] and now - last_retry >= 1.0:
                last_retry = now; _bf_sync()   # g_avatar 还没就绪时每秒重试
                _bf_write_status()
            if now - last_status >= 1.0:       # ~1s 心跳，供叠加层判断在线/就绪
                last_status = now; _bf_write_status()
        except Exception:
            _err("bf_loop")
        time.sleep(0.10)

# ---------- 启动 ----------
try:
    _w(ERR, "probe2+beautify boot %.1f py=%s\n" % (time.time(), sys.version.split()[0]), "a")
    _t = threading.Thread(target=loop); _t.daemon = True; _t.start()
    _bt = threading.Thread(target=beautify_control_loop); _bt.daemon = True; _bt.start()
    _w(ERR, "probe2+beautify thread started\n", "a")
except Exception:
    _err("boot")
