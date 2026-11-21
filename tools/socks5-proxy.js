#!/usr/bin/env node
'use strict';
/**
 * 极简 SOCKS5 代理服务器（纯 Node 内置模块，零第三方依赖）
 *
 * 运行方式（借用豆包的 Electron 内置 Node）：
 *   set ELECTRON_RUN_AS_NODE=1
 *   "D:\Application\Doubao\app\Doubao.exe" tools\socks5-proxy.js
 *
 * 参数：
 *   --port 1080                 监听端口（默认 1080）
 *   --host 127.0.0.1            监听地址（默认只监听本机，安全）
 *   --upstream http://[user:pass@]host:port
 *                               可选：把流量再转交给一个上游代理（目前支持 http/https）
 *   -v                          打印每个连接的调试日志
 *
 * 成功后给 git 配置：
 *   git config --global http.proxy  socks5h://127.0.0.1:1080
 *   git config --global https.proxy socks5h://127.0.0.1:1080
 */

const net = require('net');

const ATYP_IPV4 = 0x01;
const ATYP_DOMAIN = 0x03;
const ATYP_IPV6 = 0x04;

const REP_OK = 0x00;
const REP_FAIL = 0x01;
const REP_NET_UNREACH = 0x03;
const REP_HOST_UNREACH = 0x04;
const REP_REFUSED = 0x05;
const REP_CMD_UNSUPPORTED = 0x07;
const REP_ATYP_UNSUPPORTED = 0x08;

const log = (...a) => console.log(`[${new Date().toLocaleTimeString()}]`, ...a);

function die(msg) {
  console.error('[错误] ' + msg);
  process.exit(1);
}

function parseArgs(argv) {
  const cfg = { port: 1080, host: '127.0.0.1', upstream: null, verbose: false };
  for (let i = 0; i < argv.length; i++) {
    let key = argv[i];
    let val = null;
    const eq = key.indexOf('=');
    if (eq > 0) {
      val = key.slice(eq + 1);
      key = key.slice(0, eq);
    }
    const takeVal = () => {
      if (val !== null) return val;
      if (i + 1 >= argv.length) die(`参数 ${key} 缺少取值`);
      return argv[++i];
    };
    switch (key) {
      case '--port': cfg.port = Number(takeVal()); break;
      case '--host': cfg.host = takeVal(); break;
      case '--upstream': cfg.upstream = takeVal(); break;
      case '-v':
      case '--verbose': cfg.verbose = true; break;
      case '-h':
      case '--help':
        console.log('用法: node socks5-proxy.js [--host 127.0.0.1] [--port 1080] [--upstream http://host:port] [-v]');
        process.exit(0);
        break;
      default:
        die(`未知参数: ${key}（用 --help 查看用法）`);
    }
  }
  if (!Number.isInteger(cfg.port) || cfg.port <= 0 || cfg.port > 65535) die('--port 不合法');
  return cfg;
}

function parseUpstream(str) {
  let u;
  try {
    u = new URL(str);
  } catch (e) {
    die(`--upstream 格式不对: ${str}，示例 http://127.0.0.1:7890`);
  }
  const proto = u.protocol.replace(':', '');
  if (proto !== 'http' && proto !== 'https') die('目前 --upstream 只支持 http:// 或 https://');
  return {
    proto,
    host: u.hostname,
    port: Number(u.port || (proto === 'https' ? 443 : 8080)),
    user: decodeURIComponent(u.username || ''),
    pass: decodeURIComponent(u.password || ''),
  };
}

const cfg = parseArgs(process.argv.slice(2));
const upstream = cfg.upstream ? parseUpstream(cfg.upstream) : null;

/* ---------------- 连接建立 ---------------- */

function connectDirect(host, port, cb) {
  const sock = net.connect({ host, port });
  const onErr = (err) => {
    sock.removeListener('connect', onOk);
    sock.destroy();
    cb(err);
  };
  const onOk = () => {
    sock.removeListener('error', onErr);
    cb(null, sock);
  };
  sock.once('error', onErr);
  sock.once('connect', onOk);
}

/** 通过上游 HTTP 代理发 CONNECT 隧道 */
function connectViaHttpProxy(up, host, port, cb) {
  const sock = net.connect({ host: up.host, port: up.port });
  let done = false;
  const finish = (err, s) => {
    if (done) return;
    done = true;
    sock.removeListener('data', onData);
    sock.removeListener('error', onErr);
    if (err) {
      sock.destroy();
      cb(err);
    } else cb(null, s);
  };
  const onErr = (err) => finish(err);
  let buf = Buffer.alloc(0);
  const onData = (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    const idx = buf.indexOf('\r\n\r\n');
    if (idx === -1) {
      if (buf.length > 16384) finish(new Error('上游代理响应头过长'));
      return;
    }
    const head = buf.slice(0, idx).toString('latin1');
    const rest = buf.slice(idx + 4);
    const m = /^HTTP\/1\.[01]\s+(\d{3})/.exec(head);
    if (!m) return finish(new Error('上游代理响应无法解析'));
    const code = Number(m[1]);
    if (code !== 200) return finish(new Error(`上游代理拒绝连接: HTTP ${code}`));
    if (rest.length) sock.unshift(rest);
    finish(null, sock);
  };
  sock.once('error', onErr);
  sock.once('connect', () => {
    const hostPort = net.isIPv6(host) ? `[${host}]:${port}` : `${host}:${port}`;
    let req = `CONNECT ${hostPort} HTTP/1.1\r\nHost: ${hostPort}\r\nProxy-Connection: Keep-Alive\r\n`;
    if (up.user) {
      const token = Buffer.from(`${up.user}:${up.pass}`).toString('base64');
      req += `Proxy-Authorization: Basic ${token}\r\n`;
    }
    req += '\r\n';
    sock.write(req, 'latin1');
    sock.on('data', onData);
  });
}

function connectTarget(host, port, cb) {
  if (upstream) connectViaHttpProxy(upstream, host, port, cb);
  else connectDirect(host, port, cb);
}

function errToRep(err) {
  switch (err && err.code) {
    case 'ECONNREFUSED': return REP_REFUSED;
    case 'ENETUNREACH':
    case 'ENETDOWN': return REP_NET_UNREACH;
    case 'EHOSTUNREACH':
    case 'ENOTFOUND':
    case 'EAI_AGAIN':
    case 'EAI_FAIL': return REP_HOST_UNREACH;
    default: return REP_FAIL;
  }
}

/* ---------------- SOCKS5 协议处理 ---------------- */

const ST_HELLO = 0;
const ST_REQUEST = 1;
const ST_PIPE = 2;

function reply(client, rep) {
  // BND.ADDR/BND.PORT 固定回 0.0.0.0:0，绝大多数客户端不关心
  client.write(Buffer.from([0x05, rep, 0x00, ATYP_IPV4, 0, 0, 0, 0, 0, 0]));
}

net.createServer({ allowHalfOpen: false }, (client) => {
  client.setNoDelay(true);

  let state = ST_HELLO;
  let buf = Buffer.alloc(0);
  let peer = '?';

  const onClientData = (chunk) => {
    buf = Buffer.concat([buf, chunk]);

    for (;;) {
      if (state === ST_HELLO) {
        if (buf.length < 2) return;
        if (buf[0] !== 0x05) {
          if (cfg.verbose) log(`[${peer}] 不是 SOCKS5 请求（ver=${buf[0]}），断开`);
          return client.destroy();
        }
        const nMethods = buf[1];
        if (buf.length < 2 + nMethods) return;
        const methods = buf.slice(2, 2 + nMethods);
        buf = buf.slice(2 + nMethods);
        if (!methods.includes(0x00)) {
          client.end(Buffer.from([0x05, 0xff])); // 不支持任何可接受的方法
          return;
        }
        client.write(Buffer.from([0x05, 0x00])); // 选择“无认证”
        state = ST_REQUEST;
        continue;
      }

      if (state === ST_REQUEST) {
        if (buf.length < 4) return;
        if (buf[0] !== 0x05) return client.destroy();
        const cmd = buf[1];
        const atyp = buf[3];

        if (cmd !== 0x01) { // 只支持 CONNECT
          if (cfg.verbose) log(`[${peer}] 不支持的 CMD ${cmd}`);
          reply(client, REP_CMD_UNSUPPORTED);
          return client.end();
        }

        let host;
        let need;
        if (atyp === ATYP_IPV4) {
          need = 4 + 4 + 2;
          if (buf.length < need) return;
          host = `${buf[4]}.${buf[5]}.${buf[6]}.${buf[7]}`;
        } else if (atyp === ATYP_DOMAIN) {
          if (buf.length < 5) return;
          const len = buf[4];
          need = 4 + 1 + len + 2;
          if (buf.length < need) return;
          host = buf.slice(5, 5 + len).toString('latin1');
        } else if (atyp === ATYP_IPV6) {
          need = 4 + 16 + 2;
          if (buf.length < need) return;
          const seg = [];
          for (let i = 0; i < 8; i++) seg.push(buf.readUInt16BE(4 + i * 2).toString(16));
          host = seg.join(':');
        } else {
          reply(client, REP_ATYP_UNSUPPORTED);
          return client.end();
        }

        const port = buf.readUInt16BE(need - 2);
        const rest = buf.slice(need);
        buf = Buffer.alloc(0);

        peer = `${host}:${port}`;
        if (cfg.verbose) log(`[${peer}] 请求连接${upstream ? `（上游 ${upstream.host}:${upstream.port}）` : ''}`);

        // 关键：先摘掉监听并暂停，否则握手过程中到达的数据会滞留在 buf 里丢掉
        state = ST_PIPE;
        client.removeListener('data', onClientData);
        client.pause();

        connectTarget(host, port, (err, target) => {
          if (err) {
            if (cfg.verbose) log(`[${peer}] 连接失败: ${err.message}`);
            reply(client, errToRep(err));
            client.resume();
            client.end();
            return;
          }
          reply(client, REP_OK);
          if (cfg.verbose) log(`[${peer}] 已连接`);

          if (rest.length) target.write(rest);
          client.pipe(target);
          target.pipe(client);
          client.resume();
          client.on('error', () => target.destroy());
          target.on('error', () => client.destroy());
        });
        return;
      }

      return; // ST_PIPE 后不再解析
    }
  };

  client.on('data', onClientData);
  client.on('error', () => {});
}).on('error', (err) => {
  if (err.code === 'EADDRINUSE') die(`端口 ${cfg.port} 已被占用，换一个 --port`);
  die(err.message);
}).listen(cfg.port, cfg.host, () => {
  log(`SOCKS5 代理已启动: socks5://${cfg.host}:${cfg.port}`);
  if (upstream) log(`上游代理: ${upstream.proto}://${upstream.host}:${upstream.port}`);
  log('git 配置: git config --global http.proxy socks5h://' + cfg.host + ':' + cfg.port);
  log('按 Ctrl+C 退出');
});

process.on('SIGINT', () => {
  log('正在退出...');
  process.exit(0);
});
