# 🌀 Azure 64-Core ARM64 Performance Benchmarking Guide

This guide provides the Infrastructure-as-Code (IaC) scripting and architecture setup required to provision a high-performance, low-latency **64-core ARM64 instance** on Microsoft Azure. It is tailored specifically for executing `io_uring` database stress testing against RingDB.

---

## 🏗️ Architecture Design & Specifications

* **Compute Topology:** `Standard_D64plsv5` (64 True Physical ARM64 Cores, 128 GiB RAM, 2 GiB RAM/vCPU). This profile bypasses traditional x86 hyper-threading cache line thrashing.
* **Network Pipeline:** SR-IOV Accelerated Networking enabled to ensure hypervisor-bypassing, sub-millisecond packet processing.
* **Pricing Engine:** Azure Spot Instances are utilized to reduce deployment costs by 80% to 90% (~$0.17 to $0.35/hr depending on market variations).
* **Target OS:** Ubuntu 24.04 LTS (delivering a native Linux Kernel 6.8+ out-of-the-box for comprehensive `io_uring` multishot feature compatibility).

---

## 📋 Prerequisites & Account Setup

Because a standard Azure account restricts low-priority allocations by default, you must complete these validation gates prior to script execution:

1. **Subscription Elevation:** Your Azure Subscriptions must be upgraded to **Pay-As-You-Go** tier. (Free Trials block large-core Spot deployments).
2. **Resource Group Registration:** Ensure core engines are active by executing:
   ```powershell
   az provider register --namespace Microsoft.Compute
   az provider register --namespace Microsoft.Network
   ```
3. **Core Quota Bump:** Navigate to **Azure Portal -> Quotas -> Compute** and request an automated limit expansion to **64 vCPUs** for:
   * `Total Regional Spot vCPUs` (Region: `centralindia`)
   * `Standard DPLSv5 Family vCPUs` (Region: `centralindia`)

---

## 🚀 Provisioning Playbook (IaC)

Execute this unified script block within a local **Windows 11 PowerShell terminal** to automatically compile the Resource Container, Virtual Network topologies, dedicated Firewall Groups, and launch the 64-Core Engine.

```powershell
# ==============================================================================
# RINGDB 64-CORE PRODUCTION BENCHMARK IACO PLAYBOOK
# ==============================================================================

# 1. Define Production Configurations
\$RESOURCE_GROUP="RingDB-Production-RG"
\$LOCATION="centralindia"  # Lowest cost region for ARM compute clusters
\$VM_NAME="RingDB-Prod-64C-VM"
\$VM_SIZE="Standard_D64plsv5"  # 64 True Physical ARM Cores / 128 GiB RAM
\$IMAGE="Canonical:ubuntu-24_04-lts:server:latest"  # Modern kernel for io_uring
\$ADMIN_USER="benchmarker"

# 2. Create the Production Container
Write-Host "Creating resource group..." -ForegroundColor Cyan
az group create --name \(RESOURCE_GROUP --location\)LOCATION

# 3. Provision the Low-Latency Network Architecture
Write-Host "Configuring high-velocity virtual network infrastructure..." -ForegroundColor Cyan
az network vnet create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Prod-VNet" `
  --address-prefixes "10.0.0.0/16" `
  --subnet-name "ProdSubnet" `
  --subnet-prefixes "10.0.1.0/24"

az network public-ip create `
  --resource-group \$RESOURCE_GROUP `
  --name "RingDB-Prod-PublicIP" `
  --sku Standard

# 4. Create and Configure Production Firewall (NSG)
Write-Host "Creating Network Security Group and opening SSH port 22..." -ForegroundColor Cyan
az network nsg create `
  --resource-group $RESOURCE_GROUP `
  --name "RingDB-Prod-NSG" `
  --location $LOCATION

az network nsg rule create `
  --resource-group \$RESOURCE_GROUP `
  --nsg-name "RingDB-Prod-NSG" `
  --name "AllowSSHInbound" `
  --protocol Tcp `
  --priority 1000 `
  --destination-port-ranges 22 `
  --access Allow `
  --direction Inbound

# 5. Bind Network Interface with Accelerated Networking & Firewall
Write-Host "Assembling network interface card..." -ForegroundColor Cyan
az network nic create `
  --resource-group \$RESOURCE_GROUP `
  --name "RingDB-Prod-NIC" `
  --vnet-name "RingDB-Prod-VNet" `
  --subnet "ProdSubnet" `
  --public-ip-address "RingDB-Prod-PublicIP" `
  --network-security-group "RingDB-Prod-NSG" `
  --accelerated-networking true  # Bypasses hypervisor for ultra-low latency

# 6. Deploy the 64-Core ARM64 Spot Virtual Machine
Write-Host "Requesting 64-Core ARM Spot VM Capacity. This may take 2-3 minutes..." -ForegroundColor Cyan
az vm create `
  --resource-group $RESOURCE_GROUP `
  --name \$VM_NAME `
  --size $VM_SIZE `
  --image \$IMAGE `
  --nics "RingDB-Prod-NIC" `
  --admin-username \$ADMIN_USER `
  --generate-ssh-keys `
  --priority Spot `
  --max-price -1 `
  --eviction-policy Delete

Write-Host "==============================================================================" -ForegroundColor Green
Write-Host "DEPLOYMENT COMPLETE! Check the JSON output above for your publicIpAddress." -ForegroundColor Green
Write-Host "==============================================================================" -ForegroundColor Green
```

---

## 🛠️ Environment Configuration & Code Compilation

Once the script outputs execution values, map the printed `"publicIpAddress"` to connect securely via SSH:

```powershell
ssh benchmarker@<YOUR_NEW_PRODUCTION_PUBLIC_IP>
```

Execute this consolidated code block inside the VM console to populate dependencies and initialize compiler modules:

```bash
# 1. Update operating system packages and deploy development headers
sudo apt update && sudo apt install -y build-essential git cmake liburing-dev htop

# 2. Compile memtier_benchmark client engine
git clone https://github.com
cd memtier_benchmark
autoreconf -ivf && ./configure && make && sudo make install
cd ~

# 3. Clone and build RingDB
git clone https://github.com/anjulanka/RingDB
cd RingDB
mkdir build && cd build
cmake ..
make -j64
```

---

## 🧽 Clean-Up & Decommissioning

To guarantee you do not incur ongoing computing fees or idle disk storage fees once your testing cycles conclude, run this single block from your local **Windows terminal** to instantly wipe out the deployment sandbox. 

*(Note: Keeping your 64-core quota limit profile active inside the portal does not trigger fees while the servers are deleted.)*

```powershell
az group delete --name "RingDB-Production-RG" --yes --no-wait
```
