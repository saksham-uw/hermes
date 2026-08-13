# Device certificates (local)

Run from repo root:

```bash
npm run provision:iot
```

That writes:

- `device.cert.pem`
- `device.private.key`
- `device.public.key`
- `AmazonRootCA1.pem`

These files are gitignored (except this README / `.gitkeep`). Do not commit live keys.
