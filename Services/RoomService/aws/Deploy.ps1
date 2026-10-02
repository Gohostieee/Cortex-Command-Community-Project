param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z][A-Za-z0-9-]{0,127}$')][string]$StackName,
    [Parameter(Mandatory)][ValidatePattern('^[a-z]{2}(-[a-z]+)+-[0-9]+$')][string]$Region,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$')][string]$ArtifactBucket,
    [Parameter(Mandatory)][string]$PlayersCIDR,
    [ValidateSet('t3.small','t3.medium','t3.large','t3.xlarge')][string]$InstanceType = 't3.small',
    [ValidateRange(1,1000)][int]$MaxRooms = 100,
    [ValidateRange(1,10000)][int]$MaxMbps = 800,
    [ValidateRange(1024,65535)][int]$ServicePort = 8001,
    [switch]$Execute
)
$ErrorActionPreference = 'Stop'
# Default creates a reviewable change set; -Execute explicitly provisions resources.
$parts = $PlayersCIDR.Split('/')
$parsedIP = $null
if ($parts.Count -ne 2 -or ![System.Net.IPAddress]::TryParse($parts[0], [ref]$parsedIP) -or $parsedIP.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork -or $parts[1] -notmatch '^([0-9]|[12][0-9]|3[0-2])$') { throw 'PlayersCIDR must be a valid IPv4 CIDR.' }
$package = & (Join-Path (Split-Path -Parent $PSScriptRoot) 'Package.ps1')
& aws s3api head-bucket --bucket $ArtifactBucket --region $Region
if ($LASTEXITCODE) { throw 'The private artifact bucket is unavailable.' }
$publicAccess = & aws s3api get-public-access-block --bucket $ArtifactBucket --region $Region --query PublicAccessBlockConfiguration --output json
if ($LASTEXITCODE) { throw 'Enable all four bucket public-access blocks before deployment.' }
$publicAccess = $publicAccess | ConvertFrom-Json
if (!$publicAccess.BlockPublicAcls -or !$publicAccess.IgnorePublicAcls -or !$publicAccess.BlockPublicPolicy -or !$publicAccess.RestrictPublicBuckets) { throw 'The artifact bucket must have all four public-access blocks enabled.' }
& aws s3 cp $package.Archive "s3://$ArtifactBucket/$($package.Key)" --sse AES256 --region $Region --only-show-errors
if ($LASTEXITCODE) { throw 'Source upload failed.' }
$template = Join-Path $PSScriptRoot 'room-service.yaml'
& aws cloudformation validate-template --template-body "file://$template" --region $Region --query Description --output text
if ($LASTEXITCODE) { throw 'AWS rejected the template.' }
$arguments = @('cloudformation', 'deploy', '--template-file', $template, '--stack-name', $StackName, '--region', $Region, '--capabilities', 'CAPABILITY_IAM', '--parameter-overrides', "ArtifactBucket=$ArtifactBucket", "ArtifactKey=$($package.Key)", "ArtifactSha256=$($package.Sha256)", "PlayersCIDR=$PlayersCIDR", "InstanceType=$InstanceType", "MaxRooms=$MaxRooms", "MaxMbps=$MaxMbps", "ServicePort=$ServicePort")
if (!$Execute) { $arguments += '--no-execute-changeset' }
& aws @arguments
if ($LASTEXITCODE) { throw 'CloudFormation deployment failed. Inspect its validation results and events.' }
if ($Execute) {
    & aws cloudformation describe-stacks --stack-name $StackName --region $Region --query 'Stacks[0].Outputs' --output table
    if ($LASTEXITCODE) { throw 'Could not read deployment outputs.' }
} else { Write-Output 'Change set prepared. Review it before execution. No EC2 server has been launched.' }
