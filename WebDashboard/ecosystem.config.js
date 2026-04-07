// PM2 process manager config
// Usage:  pm2 start ecosystem.config.js
//         pm2 save && pm2 startup   (persist across reboots)

module.exports = {
  apps: [
    {
      name        : 'predictivEdge',
      script      : 'server.js',
      cwd         : __dirname,
      instances   : 1,
      autorestart : true,
      watch       : false,
      max_memory_restart: '256M',
      env: {
        NODE_ENV  : 'production',
        HOST      : '0.0.0.0',
        HTTP_PORT : 3000,
        TCP_PORT  : 3001
      },
      error_file  : './logs/err.log',
      out_file    : './logs/out.log',
      log_date_format: 'YYYY-MM-DD HH:mm:ss'
    }
  ]
};
