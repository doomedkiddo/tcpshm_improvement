# Market Data Generator and Consumer

This project demonstrates how to use the TCPSHM framework to efficiently transfer market data between a server and client using shared memory (SHM) or TCP.

## Overview

The server generates five types of market data for 120 cryptocurrency pairs:
1. **Market Depth Data**: 5 levels of bid/ask prices and quantities
2. **Trade Data**: Individual trade execution details
3. **GARCH Volatility Data**: Volatility measurements and forecasts
4. **Candlestick Data**: OHLC price information
5. **Ticker Data**: 24-hour summary statistics

The client connects to the server and receives this data, counts it, and displays statistics every 5 seconds.

## Usage

### Building
Run `./build_cmake.sh` to build the project using CMake.

### Running the Server
```bash
./echo_server
```

The server will:
- Listen on 0.0.0.0:12345
- Generate market data for all 120 pairs
- Send the data to connected clients

### Running the Client
```bash
./echo_client CLIENT_NAME SERVER_IP USE_SHM
```

Parameters:
- `CLIENT_NAME`: A unique identifier for this client
- `SERVER_IP`: The IP address of the server (e.g., 127.0.0.1)
- `USE_SHM`: Use 1 for shared memory mode (faster) or 0 for TCP mode

Example:
```bash
./echo_client client1 127.0.0.1 1
```

The client will:
- Connect to the server
- Process all incoming market data
- Display statistics every 5 seconds

## Performance

Shared memory mode provides significantly lower latency than TCP mode and is recommended for production use when the client and server are on the same machine.

## Customization

You can customize the market data generation in the server by modifying the following methods:
- `GenerateMarketDepth`
- `GenerateTrade`
- `GenerateGarchVolatility`
- `GenerateCandlestick`
- `GenerateTicker`

Similarly, you can extend the client's data handling in:
- `handleMarketDepthData`
- `handleTradeData`
- `handleGarchVolatilityData`
- `handleCandlestickData`
- `handleTickerData`
