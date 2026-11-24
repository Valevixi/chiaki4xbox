# Creates the self-signed test certificate the package is signed with (app\ChiakiXbox_TemporaryKey.pfx).
# Run once in PowerShell from this folder:  powershell -ExecutionPolicy Bypass -File make-cert.ps1
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=ChiakiXbox" -KeyUsage DigitalSignature `
  -FriendlyName "chiaki4xbox test cert" -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
$pwd = New-Object System.Security.SecureString
Export-PfxCertificate -Cert $cert -FilePath "$PSScriptRoot\app\ChiakiXbox_TemporaryKey.pfx" -Password $pwd | Out-Null
Write-Host "Wrote app\ChiakiXbox_TemporaryKey.pfx"
