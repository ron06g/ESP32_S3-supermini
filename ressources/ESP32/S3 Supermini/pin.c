// définir la led selon le schéma des broches

int led = 8 ;



configuration annulaire() {

// initialiser la LED de la broche numérique comme une sortie

pinMode (led, SORTIE) ;

}



boucle vide() {

digitalWrite (led, HIGH) ; // allume la LED

délai(1000); //attendre une seconde

digitalWrite (led, FAIBLE) ; // éteint la LED

délai(1000); //attendre une seconde

}


PWM numérique

Téléchargez le code suivant pour voir la LED intégrée s'assombrit progressivement.



int ledPin = 8 ; // LED connectée à la broche numérique 10



configuration annulaire() {

// déclarant la broche LED comme sortie

pinMode (ledPin, SORTIE) ;

}



boucle vide() {

// s'estompe de min à max par incréments de 5 points :

pour (int fonduValue = 0 ; fonduValue <= 255 ; fonduValue += 5) {

// définit la valeur (plage de 0 à 255) :

analogWrite (ledPin, fonduValue);

//attendre 30 millisecondes pour voir l'effet de gradation

délai (30);

}



// s'estompe de max à min par incréments de 5 points :

pour (int fonduValue = 255 ; fonduValue >= 0 ; fonduValue -= 5) {

// définit la valeur (plage de 0 à 255) :

analogWrite (ledPin, fonduValue);

//attendre 30 millisecondes pour voir l'effet de gradation

délai (30);

}

}


Broche analogique

Connectez le potentiomètre à la broche A5 et téléchargez le code suivant pour contrôler l'intervalle de clignotement de la LED en tournant le bouton du potentiomètre.



const int sensorPin = A5 ;

const int ledPin = 8 ;



configuration annulaire() {

pinMode (capteurPin, ENTRÉE) ; // déclarer le capteurPin comme une ENTRÉE

pinMode (ledPin, SORTIE) ; // déclarer le ledPin comme une SORTIE

}



boucle vide() {

// lire la valeur du capteur :

int sensorValue = analogRead(sensorPin);

// allume le ledPin

digitalWrite (ledPin, HIGH);

// arrêter le programme pour <sensorValue> millisecondes :

délai (valeur du capteur) ;

// éteint la ledPin :

digitalWrite (ledPin, FAIBLE);

// arrêter le programme pour <sensorValue> millisecondes :

délai (valeur du capteur) ;

}

