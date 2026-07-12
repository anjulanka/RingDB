# RingDB ARM64 Benchmarking Deployment Guide

This document contains the automated Azure CLI infrastructure deployment script and a quick-reference hardware table for scaling RingDB benchmarks from 8 cores up to 64 cores on native ARM64 architecture.

## 1. Automated PowerShell Deployment Script

Copy and paste this script directly into your PowerShell terminal. To scale your deployment up or down, simply modify the parameters under **Step 1** before executing the script.

```powershell
# ==============================================================================
# RINGDB DYNAMIC ARM64 BENCHMARK IACO PLAYBOOK
# ==============================================================================

# --- CONFIGURATION LAYER (UPDATE THESE VALUES AS NEEDED) ---
$RESOURCE_GROUP     = "RingDB-Benchmark-RG"
$LOCATION           = "DenmarkEast"               
$SERVER_VM_NAME     = "RingDB-Production-Server-VM"     
$CLIENT_VM_NAME     = "RingDB-Production-Client-VM"     
$VM_SIZE            = "Standard_F4s_v2"            # Compute Optimized x64 - High Clock Speed (Fits 10-core Quota)
$IMAGE              = "Canonical:ubuntu-24_04-lts:server:latest" # Standard x64 mainline Linux platform
$SERVER_ADMIN_USER  = "server_benchmarker"
$CLIENT_ADMIN_USER  = "client_benchmarker"

# 2. Create the Resource Container
Write-Host "Creating resource group..." -ForegroundColor Cyan
az group create --name $RESOURCE_GROUP --location $LOCATION

# NEW: Create Proximity Placement Group to guarantee co-location in the same hardware rack
Write-Host "Creating Proximity Placement Group for ultra-low latency..." -ForegroundColor Cyan
az ppg create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-UltraLowLatency-PPG" `
  --location $LOCATION `
  --type Standard

# 3. Provision the Low-Latency Network Architecture
Write-Host "Configuring high-velocity virtual network infrastructure..." -ForegroundColor Cyan
az network vnet create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Bench-VNet" `
  --address-prefixes "10.0.0.0/16" `
  --subnet-name "BenchSubnet" `
  --subnet-prefixes "10.0.1.0/24"

az network public-ip create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Server-PublicIP" `
  --sku Standard

az network public-ip create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Client-PublicIP" `
  --sku Standard

# 4. Create and Configure Production Firewall (NSG)
az network nsg create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Bench-NSG" `
  --location $LOCATION

az network nsg rule create `
  --resource-group $RESOURCE_GROUP `
  --nsg-name "RingDB-Bench-NSG" `
  --name "AllowSSHInbound" `
  --protocol Tcp `
  --priority 1000 `
  --destination-port-ranges 22 `
  --access Allow `
  --direction Inbound

# Open Redis port for memtier_benchmark
az network nsg rule create `
  --resource-group $RESOURCE_GROUP `
  --nsg-name "RingDB-Bench-NSG" `
  --name "AllowRingDBTraffic" `
  --protocol Tcp `
  --priority 1010 `
  --destination-port-ranges 6379 `
  --access Allow `
  --direction Inbound

# 5. Bind Network Interfaces with Accelerated Networking
Write-Host "Assembling network interface cards with Accelerated Networking..." -ForegroundColor Cyan
az network nic create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Server-NIC" `
  --vnet-name "RingDB-Bench-VNet" `
  --subnet "BenchSubnet" `
  --public-ip-address "RingDB-Server-PublicIP" `
  --network-security-group "RingDB-Bench-NSG" `
  --accelerated-networking true

az network nic create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Client-NIC" `
  --vnet-name "RingDB-Bench-VNet" `
  --subnet "BenchSubnet" `
  --public-ip-address "RingDB-Client-PublicIP" `
  --network-security-group "RingDB-Bench-NSG" `
  --accelerated-networking true

# 6. Deploy the Targeted Standard x64 Virtual Machines (Bound to the PPG)
Write-Host "Deploying $SERVER_VM_NAME inside the PPG..." -ForegroundColor Cyan
az vm create `
  --resource-group $RESOURCE_GROUP `
  --name $SERVER_VM_NAME `
  --size $VM_SIZE `
  --image $IMAGE `
  --nics "RingDB-Server-NIC" `
  --admin-username $SERVER_ADMIN_USER `
  --ppg "RingDB-UltraLowLatency-PPG" `
  --generate-ssh-keys 

Write-Host "Deploying $CLIENT_VM_NAME inside the PPG..." -ForegroundColor Cyan
az vm create `
  --resource-group $RESOURCE_GROUP `
  --name $CLIENT_VM_NAME `
  --size $VM_SIZE `
  --image $IMAGE `
  --nics "RingDB-Client-NIC" `
  --admin-username $CLIENT_ADMIN_USER `
  --ppg "RingDB-UltraLowLatency-PPG" `
  --generate-ssh-keys 


Write-Host "==============================================================================" -ForegroundColor Green
Write-Host "DEPLOYMENT COMPLETE! Check the JSON output above for your publicIpAddress." -ForegroundColor Green
Write-Host "==============================================================================" -ForegroundColor Green
```

## 2. Linux ARM64 SKU Reference Table (8 to 64 Cores)

These SKUs utilize native **Ampere Altra ARM64 processors** (2 GiB of memory per core architecture + SSD storage cache), guaranteeing perfect alignment with your native `server-arm64` platform image choice.

| Azure VM SKU | Physical CPU Cores | Memory (GiB) | Best Use Case |
| :--- | :---: | :---: | :--- |
| **`Standard_D8pds_v5`** | 8 | 16 | Local debugging and initial staging smoke tests. |
| **`Standard_D16pds_v5`** | 16 | 32 | Mid-tier verification and multi-thread configuration testing. |
| **`Standard_D32pds_v5`** | 32 | 64 | Scale validation and network bottleneck testing. |
| **`Standard_D64pds_v5`** | 64 | 128 | High-velocity production benchmarking and stress loads. |

---
