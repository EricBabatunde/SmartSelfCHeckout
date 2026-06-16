#include "modules.h"

BarcodeScanner scanner;
InventoryDB inventory;
SystemDisplay display;
SecurityScale bagScale;
SystemIO sysIO;

enum SystemState {
  STATE_STARTUP_CHECK,
  STATE_IDLE,
  STATE_CHECKOUT_MODE,
  STATE_PROCESSING_SCAN,
  STATE_WAITING_FOR_SCALE,
  STATE_REMOVAL_REQUESTED,
  STATE_WAITING_FOR_REMOVAL,
  STATE_VIEW_CART,
  STATE_TAMPER_ALARM
};

SystemState currentState = STATE_STARTUP_CHECK;
std::vector<Item> cart;

float cartTotal = 0.0;
float stableCartWeight = 0.0; 
Item currentItem; 
unsigned long stateTimer = 0;
int cartViewIndex = 0;

void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  display.begin();
  sysIO.begin(); 
  
  display.printBootStep(0, "System Initialising.");
  inventory.begin(); 
  
  display.printBootStep(1, "Starting Scale...");
  bagScale.begin(HX711_DOUT_PIN, HX711_SCK_PIN, SCALE_CAL_FACTOR);
  
  display.printBootStep(2, "Starting Scanner...");
  scanner.beginScanner();
  
  delay(1000);
}

void loop() {
  scanner.task();
  sysIO.update(); // Keep button states updated

  switch (currentState) {
    
    // --- 5. STARTUP CHECK ---
    case STATE_STARTUP_CHECK: {
      long rawReading = bagScale.getRawValue();
      if (rawReading != 0) { // Ensure HX711 is responding
        if (abs(rawReading - SCALE_EMPTY_RAW) > 10000) { // Adjust threshold as needed
          display.showInstruction("CLEAR SCALE TO BOOT!");
          sysIO.alarmOn();
        } else {
          sysIO.alarmOff();
          bagScale.tareScale();
          display.setupActiveUI();
          currentState = STATE_IDLE;
        }
      }
      break;
    }

    // --- 1. NORMAL IDLE ---
    case STATE_IDLE: {
      float currentWt = bagScale.getCurrentWeight();
      
      // 6. IDLE TAMPERING ALARM
      if (abs(currentWt - stableCartWeight) > bagScale.getTolerance(stableCartWeight)) {
        sysIO.alarmOn();
        display.showInstruction("TAMPER DETECTED!    ");
        currentState = STATE_TAMPER_ALARM;
        break;
      }

      // Check Button Inputs
      if (sysIO.isViewPressed() && cart.size() > 0) {
        cartViewIndex = 0;
        stateTimer = millis();
        display.showCartItem(cart[0], 0, cart.size());
        currentState = STATE_VIEW_CART;
      } 
      else if (sysIO.isCheckoutDoubleClicked()) {
        display.showMode("== CHECKOUT MODE ==");
        display.showInstruction("Scan to REMOVE item");
        sysIO.successSignal();
        currentState = STATE_CHECKOUT_MODE;
      }
      else if (sysIO.isCheckoutSingleClicked()) {
        // Trigger payment sequence here
        display.showInstruction("Payment Approved.   ");
        sysIO.successSignal();
        delay(3000);
        cart.clear(); cartTotal = 0.0; stableCartWeight = 0.0;
        bagScale.tareScale(); display.setupActiveUI();
      }
      // Check Scanner
      else if (scanner.hasNewData()) {
        String scannedCode = scanner.getScannedCode();
        scannedCode.trim(); 
        currentItem = inventory.getItemByBarcode(scannedCode);
        
        if (currentItem.found) {
          display.showInstruction("Place item on scale!");
          currentState = STATE_WAITING_FOR_SCALE;
        } else {
          sysIO.errorSignal();
          display.showError();
          delay(2000); display.setupActiveUI(); display.updateCart(currentItem, cartTotal);
        }
      }
      break;
    }

    // --- 2. DYNAMIC ADDING ALARM ---
    case STATE_WAITING_FOR_SCALE: {
      float reading = bagScale.getCurrentWeight();
      float targetWeight = stableCartWeight + currentItem.weight;
      float tol = bagScale.getTolerance(currentItem.weight);

      if (abs(reading - targetWeight) <= tol) {
        // Item placed correctly
        sysIO.alarmOff(); sysIO.successSignal();
        stableCartWeight = reading;
        cartTotal += currentItem.price;
        cart.push_back(currentItem); // Add to vector
        display.updateCart(currentItem, cartTotal);
        currentState = STATE_IDLE;
      } 
      else if (abs(reading - stableCartWeight) > tol) {
        // Incorrect weight added
        sysIO.alarmOn();
        display.showInstruction("Weight Mismatch!    ");
      } 
      else {
        // Waiting for placement (weight hasn't changed yet)
        sysIO.alarmOff();
        display.showInstruction("Place item on scale!");
      }
      break;
    }

    // --- 3. CHECKOUT / REMOVAL MODE ---
    case STATE_CHECKOUT_MODE: {
      // Exit mode if double clicked again
      if (sysIO.isCheckoutDoubleClicked()) {
        display.setupActiveUI();
        display.updateCart(currentItem, cartTotal);
        currentState = STATE_IDLE;
      }
      else if (scanner.hasNewData()) {
        String scannedCode = scanner.getScannedCode();
        scannedCode.trim();
        
        // Check if item is in our cart vector
        int foundIndex = -1;
        for(size_t i=0; i < cart.size(); i++) {
          if (cart[i].sku == scannedCode) { foundIndex = i; break; }
        }

        if (foundIndex != -1) {
          currentItem = cart[foundIndex];
          // Remove from vector immediately so it doesn't show in viewer, we revert if scale fails
          cart.erase(cart.begin() + foundIndex); 
          display.showInstruction("Remove from scale!  ");
          currentState = STATE_WAITING_FOR_REMOVAL;
        } else {
          sysIO.errorSignal();
          display.showInstruction("Not in cart!        ");
          delay(2000); display.showInstruction("Scan to REMOVE item");
        }
      }
      break;
    }

    // --- 2. DYNAMIC REMOVAL ALARM ---
    case STATE_WAITING_FOR_REMOVAL: {
      float reading = bagScale.getCurrentWeight();
      float targetWeight = stableCartWeight - currentItem.weight;
      float tol = bagScale.getTolerance(currentItem.weight);

      if (abs(reading - targetWeight) <= tol) {
        sysIO.alarmOff(); sysIO.successSignal();
        stableCartWeight = reading;
        cartTotal -= currentItem.price;
        if(cartTotal < 0) cartTotal = 0;
        
        display.setupActiveUI();
        display.updateCart(currentItem, cartTotal);
        currentState = STATE_IDLE; // Return to idle after successful removal
      } 
      else if (abs(reading - stableCartWeight) > tol) {
        sysIO.alarmOn();
        display.showInstruction("Removal Mismatch!   ");
      } 
      else {
        sysIO.alarmOff();
        display.showInstruction("Remove from scale!  ");
      }
      break;
    }

    // --- 4. CART VIEWER AUTO-SCROLL ---
    case STATE_VIEW_CART: {
      if (millis() - stateTimer > 1500) { // 1.5 seconds per item
        stateTimer = millis();
        cartViewIndex++;
        
        if (cartViewIndex >= cart.size()) {
          display.setupActiveUI();
          display.updateCart(currentItem, cartTotal);
          currentState = STATE_IDLE;
        } else {
          display.showCartItem(cart[cartViewIndex], cartViewIndex, cart.size());
        }
      }
      break;
    }

    // --- 6. SECURITY LOCK ---
    case STATE_TAMPER_ALARM: {
      float currentWt = bagScale.getCurrentWeight();
      if (abs(currentWt - stableCartWeight) <= bagScale.getTolerance(stableCartWeight)) {
        sysIO.alarmOff();
        display.setupActiveUI();
        display.updateCart(currentItem, cartTotal);
        currentState = STATE_IDLE;
      }
      break;
    }
  }
}