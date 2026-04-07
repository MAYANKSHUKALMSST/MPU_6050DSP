#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  PredictivEdge — one-shot server deployment script
#  Tested on Ubuntu 22.04 / Debian 12
#
#  Run as root (or with sudo):
#    chmod +x deploy.sh && sudo ./deploy.sh
# ════════════════════════════════════════════════════════════════
set -euo pipefail

SERVER_IP="161.118.167.196"
APP_DIR="/opt/predictivEdge"
NGINX_CONF="/etc/nginx/sites-available/predictivEdge"
NODE_MAJOR=20   # Node.js LTS version

echo "════════════════════════════════════════"
echo "  PredictivEdge Deployment"
echo "  Target: $SERVER_IP"
echo "════════════════════════════════════════"

# ── 1. System dependencies ────────────────────────────────────
echo "[1/7] Installing system packages..."
apt-get update -qq
apt-get install -y -qq curl git nginx ufw

# ── 2. Node.js (via NodeSource) ───────────────────────────────
if ! command -v node &>/dev/null || [[ "$(node -e 'process.stdout.write(process.version.split(".")[0].slice(1))')" -lt "$NODE_MAJOR" ]]; then
    echo "[2/7] Installing Node.js $NODE_MAJOR..."
    curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash -
    apt-get install -y nodejs
else
    echo "[2/7] Node.js $(node --version) already installed — skipping."
fi

# ── 3. PM2 (global) ───────────────────────────────────────────
echo "[3/7] Installing PM2..."
npm install -g pm2 --quiet

# ── 4. Copy application files ─────────────────────────────────
echo "[4/7] Deploying app to $APP_DIR..."
mkdir -p "$APP_DIR"
# Copy all WebDashboard files to the app directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rsync -a --exclude='node_modules' --exclude='firmware' "$SCRIPT_DIR/" "$APP_DIR/"

# Create log and firmware directories
mkdir -p "$APP_DIR/logs"
mkdir -p "$APP_DIR/firmware"

# Install Node dependencies
cd "$APP_DIR"
npm install --omit=dev --quiet
echo "    → Dependencies installed."

# ── 5. Start / restart with PM2 ───────────────────────────────
echo "[5/7] Starting app with PM2..."
pm2 delete predictivEdge 2>/dev/null || true
pm2 start "$APP_DIR/ecosystem.config.js"
pm2 save

# Enable PM2 to start on reboot
env PATH="$PATH:/usr/bin" pm2 startup systemd -u root --hp /root | tail -1 | bash || true
echo "    → PM2 startup configured."

# ── 6. nginx reverse proxy ────────────────────────────────────
echo "[6/7] Configuring nginx..."
cp "$SCRIPT_DIR/nginx.conf" "$NGINX_CONF"
ln -sf "$NGINX_CONF" /etc/nginx/sites-enabled/predictivEdge

# Remove default site if it conflicts on port 80
rm -f /etc/nginx/sites-enabled/default

nginx -t
systemctl enable nginx --quiet
systemctl reload nginx
echo "    → nginx configured and reloaded."

# ── 7. Firewall (ufw) ─────────────────────────────────────────
echo "[7/7] Configuring firewall..."
ufw allow 22/tcp    comment "SSH"          2>/dev/null || true
ufw allow 80/tcp    comment "HTTP nginx"   2>/dev/null || true
ufw allow 3001/tcp  comment "Device TCP"   2>/dev/null || true
ufw --force enable 2>/dev/null || true
echo "    → Firewall rules applied."

# ── Summary ───────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo "  Deployment complete!"
echo ""
echo "  Dashboard → http://$SERVER_IP"
echo "  Device TCP → $SERVER_IP:3001"
echo ""
echo "  Useful commands:"
echo "    pm2 logs predictivEdge    # live logs"
echo "    pm2 status                # process status"
echo "    pm2 restart predictivEdge # restart app"
echo "    pm2 monit                 # live CPU/RAM"
echo "════════════════════════════════════════"
