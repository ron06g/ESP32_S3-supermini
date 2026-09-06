#inclure <BLEDevice.h>

#inclure <BLEUtils.h>

#inclure <BLEServer.h>



// Voir ce qui suit pour générer des UUID :

//https://www.uuidgenerator.net/



#définir SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"

#définir CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"




classe MyCallback : public BLECaractéristiqueCallback {

nul surWrite(BLECaractéristique *pCaractéristique) {

std ::valeur de la chaîne = pCaractéristique->getValue();



si (valeur.longueur() > 0) {

Série.println(""*********);

Serial.print("Nouvelle valeur : ");

pour (int i = 0 ; i < valeur.longueur(); i++)

Serial.print (valeur[i]);



Série.println();

Série.println(""*********);

}

}

};



configuration annulaire() {

Serial.start(115200);



Appareil BLED :init("MyESP32");

BLEServer *pServer = BLEDappareil ::créateServer();



BLEService *pService = pServer->créateService(SERVICE_UUID);



BLECaractéristique *pCaractéristique = pService->créerCaractéristique(

CARACTÉRISTIQUE_UUID,

BLECaractéristique ::PROPERTY_READ|

BLECaractéristique ::PROPERTY_WRITE

);



pCaractéristique->setCallback (nouveaux MyCallback());



pCaractéristique->setValeur("Bonjour Monde");

pService->start();



BLEAdvertisement *pAdvertisement = pServer->getAdvertisement();

pPublicité->start();

}



boucle vide() {

// mettez votre code principal ici pour exécuter à plusieurs reprises :

délai (2000);

}