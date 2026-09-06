#inclure <BLEDevice.h>

#inclure <BLEUtils.h>

#inclure <BLEScan.h>

#inclure <BLEAdvertisedDevice.h>



int scanHeure = 5 ; //En quelques secondes

BLEScan* pBLEScan ;



classe MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {

nul surRésultat (BLEAdvertisementDevice annoncéDevice) {

Serial.printf("Appareil publicitaire : %s \n", annoncéDevice.toString().c_str());

}

};



configuration annulaire() {

Serial.start(115200);

Serial.println(""Scan...");



Appareil BLED :init("");

pBLEScan = BLEDappareil ::getScan(); //créer une nouvelle analyse

pBLEScan->setAdvertisedDeviceCallbacks (nouveau MyAdvertisedDeviceCallbacks());

pBLEScan->setActiveScan(true); //active scan utilise plus de puissance, mais obtenez des résultats plus rapidement

pBLEScan->setIntervalle (100);

pBLEScan->setWindow(99); // réglage inférieur ou égalValeur d'intervalle

}



boucle vide() {

// mettez votre code principal ici pour exécuter à plusieurs reprises :

BLEScanResults trouvéDispositifs = pBLEScan->start(scanTime, faux);

Serial.print("Appareils trouvés : ");

Serial.println(contradoDispositifs.getCount());

Serial.println("Scan terminé!");

pBLEScan->clearResults(); // supprimer les résultats du tampon BLEScan pour libérer la mémoire

délai (2000);

}

