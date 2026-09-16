"""
Shopify probe for the ESP32 order notifier.
Tests the client credentials grant, then fetches recent orders and prints
the response size (used later to size the ESP32's HTTP receive buffer).

Credentials come from environment variables ONLY. Never hardcode them.
  set SHOPIFY_SHOP=yourstore          (the part before .myshopify.com)
  set SHOPIFY_CLIENT_ID=...
  set SHOPIFY_CLIENT_SECRET=...
  python probe.py
"""
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

API_VERSION = "2026-07"


def need(name):
    value = os.environ.get(name, "").strip()
    if not value:
        sys.exit(f"Missing environment variable {name}. Run: set {name}=...")
    return value


def post(url, data, headers):
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def main():
    shop = need("SHOPIFY_SHOP")
    client_id = need("SHOPIFY_CLIENT_ID")
    client_secret = need("SHOPIFY_CLIENT_SECRET")
    base = f"https://{shop}.myshopify.com"

    # Step 1: exchange client credentials for an access token
    print(f"[1] Requesting access token from {shop}.myshopify.com ...")
    form = urllib.parse.urlencode({
        "grant_type": "client_credentials",
        "client_id": client_id,
        "client_secret": client_secret,
    }).encode()
    status, body = post(
        f"{base}/admin/oauth/access_token",
        form,
        {"Content-Type": "application/x-www-form-urlencoded"},
    )
    if status != 200:
        print(f"    FAILED (HTTP {status}):")
        print("    " + body.decode(errors="replace")[:500])
        sys.exit(1)

    token_info = json.loads(body)
    token = token_info["access_token"]
    print(f"    OK. Token expires in {token_info.get('expires_in')} seconds.")
    print(f"    Scopes granted: {token_info.get('scope')}")

    # Step 2: fetch recent orders with a minimal GraphQL query
    print("[2] Fetching the 5 most recent orders ...")
    query = """
    {
      orders(first: 5, sortKey: CREATED_AT, reverse: true) {
        edges {
          node {
            name
            createdAt
            displayFinancialStatus
            displayFulfillmentStatus
            totalPriceSet { shopMoney { amount currencyCode } }
          }
        }
      }
    }
    """
    status, body = post(
        f"{base}/admin/api/{API_VERSION}/graphql.json",
        json.dumps({"query": query}).encode(),
        {"Content-Type": "application/json", "X-Shopify-Access-Token": token},
    )
    print(f"    HTTP {status}, response size: {len(body)} bytes")

    result = json.loads(body)
    if "errors" in result:
        print("    GraphQL errors:")
        print("    " + json.dumps(result["errors"], indent=2)[:800])
        sys.exit(1)

    edges = result["data"]["orders"]["edges"]
    if not edges:
        print("    No orders found in the last ~60 days.")
    for edge in edges:
        o = edge["node"]
        money = o["totalPriceSet"]["shopMoney"]
        print(f"    {o['name']:>8}  {o['createdAt']}  "
              f"{money['amount']} {money['currencyCode']}  "
              f"{o['displayFinancialStatus']} / {o['displayFulfillmentStatus']}")

    print("\nDone. Note the response size above for the ESP32 buffer.")


if __name__ == "__main__":
    main()
