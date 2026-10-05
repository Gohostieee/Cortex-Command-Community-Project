param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z][A-Za-z0-9-]{0,127}$')][string]$StackName,
    [Parameter(Mandatory)][ValidatePattern('^[a-z]{2}(-[a-z]+)+-[0-9]+$')][string]$Region,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$')][string]$ArtifactBucket,
    [Parameter(Mandatory)][string]$RuntimeArchive,
    [Parameter(Mandatory)][string]$PlayersCIDR,
    [ValidateSet('c6i.large','c6i.xlarge','c7i.large','c7i.xlarge','c7i.2xlarge','c7a.large','c7a.xlarge','c7a.2xlarge')][string]$InstanceType = 'c7i.xlarge',
    [ValidateRange(1,8)][int]$MaxHosted = 2,
    [ValidateRange(1024,65535)][int]$ServicePort = 8001,
    [ValidateRange(1024,65527)][int]$GamePortBase = 8100,
    [switch]$Execute
)
$ErrorActionPreference = 'Stop'
$parts = $PlayersCIDR.Split('/')
$parsedIP = $null
if ($parts.Count -ne 2 -or ![System.Net.IPAddress]::TryParse($parts[0], [ref]$parsedIP) -or $parsedIP.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork -or $parts[1] -notmatch '^([0-9]|[12][0-9]|3[0-2])$') { throw 'PlayersCIDR must be a valid IPv4 CIDR.' }
$gamePortEnd = $GamePortBase + $MaxHosted - 1
if ($ServicePort -ge $GamePortBase -and $ServicePort -le $gamePortEnd) { throw 'The broker port must not overlap game worker ports.' }
$RuntimeArchive = (Resolve-Path -LiteralPath $RuntimeArchive).Path
$releaseText = & tar -xzOf $RuntimeArchive './release.json'
if ($LASTEXITCODE) { throw 'Runtime archive has no dedicated release manifest.' }
$release = $releaseText | ConvertFrom-Json
if ($release.schema -ne 1 -or $release.platform -ne 'ubuntu-24.04-x86_64') { throw 'Deploy a dedicated Ubuntu 24.04 x86_64 runtime produced by the build script.' }
$sha = (Get-FileHash -LiteralPath $RuntimeArchive -Algorithm SHA256).Hash.ToLowerInvariant()
$artifactKey = "dedicated-server/releases/$sha.tar.gz"
& aws s3api head-bucket --bucket $ArtifactBucket --region $Region
if ($LASTEXITCODE) { throw 'The artifact bucket is unavailable.' }
$access = & aws s3api get-public-access-block --bucket $ArtifactBucket --region $Region --query PublicAccessBlockConfiguration --output json
if ($LASTEXITCODE) { throw 'Enable all four bucket public-access blocks before deployment.' }
$access = $access | ConvertFrom-Json
if (!$access.BlockPublicAcls -or !$access.IgnorePublicAcls -or !$access.BlockPublicPolicy -or !$access.RestrictPublicBuckets) { throw 'All four artifact bucket public-access blocks are required.' }
& aws s3 cp $RuntimeArchive "s3://$ArtifactBucket/$artifactKey" --sse AES256 --region $Region --only-show-errors
if ($LASTEXITCODE) { throw 'Dedicated runtime upload failed.' }
$template = Join-Path $PSScriptRoot 'dedicated-server.yaml'
& aws cloudformation validate-template --template-body "file://$template" --region $Region --query Description --output text
if ($LASTEXITCODE) { throw 'AWS rejected the dedicated server template.' }
$arguments = @('cloudformation','deploy','--template-file',$template,'--stack-name',$StackName,'--region',$Region,'--capabilities','CAPABILITY_IAM','--parameter-overrides',"ArtifactBucket=$ArtifactBucket","ArtifactKey=$artifactKey","ArtifactSha256=$sha","PlayersCIDR=$PlayersCIDR","InstanceType=$InstanceType","MaxHosted=$MaxHosted","ServicePort=$ServicePort","GamePortBase=$GamePortBase","GamePortEnd=$gamePortEnd")
if (!$Execute) { $arguments += '--no-execute-changeset' }
& aws @arguments
if ($LASTEXITCODE) { throw 'Dedicated server change-set/deployment failed. Inspect CloudFormation validation events.' }
if ($Execute) {
    & aws cloudformation describe-stacks --stack-name $StackName --region $Region --query 'Stacks[0].Outputs' --output table
    if ($LASTEXITCODE) { throw 'Could not read dedicated server outputs.' }
} else {
    Write-Output 'Dedicated server change set prepared; no EC2 resources were launched.'
}
