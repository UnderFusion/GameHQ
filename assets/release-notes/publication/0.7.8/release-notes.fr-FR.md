# GameHQ 0.7.8 (2026-09-22)

## Points forts

- La surcouche fonctionne désormais de manière bien plus fiable au-dessus des jeux en fenêtré sans bordure. Elle s’affiche au-dessus du jeu, capte les commandes de la manette pour la navigation dans la surcouche sur les chemins GameInput pris en charge, et le jeu reste visible. Vérifié avec une DualSense filaire dans Indiana Jones and the Great Circle en mode fenêtré sans bordure (Borderless).
- Sur les chemins GameInput pris en charge, naviguer dans la surcouche ne contrôle plus le jeu en dessous. À la fermeture, la surcouche attend que vous relâchiez tous les boutons maintenus avant de rendre la manette au jeu.
- Les clips de replay sont plus sûrs : des enregistrements rapides et répétés n’écrasent jamais un clip existant, et vous pouvez sauvegarder un clip dès le début de l’enregistrement.
- Nouveaux préréglages d’affectation : créez et modifiez des configurations de manette, puis attribuez-les à chaque manette et à chaque jeu.
- GameHQ se souvient de l’endroit où vous vous êtes arrêté : la dernière page, la catégorie des Paramètres, le filtre de la galerie et la catégorie de surcouche de chaque jeu.

## Surcouche

- La surcouche peut prendre le focus de la manette tout en restant au-dessus d’un jeu sans bordure visible, sans le réduire.
- Sur les chemins GameInput pris en charge, naviguer dans la surcouche ne contrôle plus aussi le jeu.
- À la fermeture, la surcouche attend brièvement que les boutons maintenus soient relâchés et que les sticks reviennent au repos avant de rendre la manette, afin qu’une commande maintenue ne se prolonge pas dans le jeu.
- L’ouverture et la fermeture de la surcouche sont plus rapides et plus fiables, et un nouvel appui rapide ne la referme plus juste après son ouverture. Alt-Tab et les autres basculements volontaires vers une autre application sont respectés.
- La surcouche suit le bon jeu lorsque celui-ci recrée sa fenêtre, la perd ou perd brièvement le focus. Elle se ferme proprement lorsqu’une autre application prend le relais.
- La surcouche se rouvre sur la capture d’écran ou le clip sélectionné en dernier. Après une nouvelle capture, elle démarre sur l’élément le plus récent.
- Les clips démarrent dans la surcouche sans que l’aperçu disparaisse brièvement.
- L’arrière-plan assombri apparaît désormais en même temps que les menus de la surcouche, au lieu d’apparaître en fondu après eux.
- Les indications de commandes en bas de l’écran reposent désormais sur un petit fond qui suit votre réglage d’assombrissement de la surcouche, afin de rester lisibles par-dessus le jeu.
- Le jeu auquel vous jouez actuellement est désormais correctement mis en évidence lorsque vous parcourez la barre latérale.
- La navigation à la manette fonctionne mieux dans les menus de la surcouche, la galerie et la lecture vidéo, et les boutons de navigation restent distincts des raccourcis de capture.

## Capture et replay

- Les enregistrements rapides et répétés reçoivent des noms de fichier uniques au lieu d’écraser un clip existant. Un export qui échoue ne supprime jamais les clips précédents, et les miniatures correspondent toujours au bon clip.
- L’enregistrement d’un replay utilise les images capturées jusque-là, même avant que la durée de tampon définie soit atteinte.
- Un export de replay conserve les images dont il a besoin lorsque vous changez de jeu ou que le tampon redémarre, et GameHQ attend la fin d’un export en cours avant de se fermer.
- Les captures d’écran et les enregistrements de replay sont confirmés immédiatement, puis indiquent clairement s’ils ont réussi ou échoué. Les notifications se mettent à jour sur place, et seul un nombre limité s’affiche à la fois.
- Les captures qui échouent ou sont ignorées indiquent désormais pourquoi, même lorsque les notifications de réussite sont désactivées.
- L’état du replay indique désormais si l’enregistrement a réellement commencé et si des images exploitables sont disponibles, avec des messages clairs lorsque le tampon démarre, est vide ou qu’un export est en cours.
- Les sessions de replay manuelles résistent désormais aux modifications des paramètres et aux captures d’écran HDR prises en même temps. Une session manuelle démarrée mais jamais utilisée se désactive au bout d’un moment.
- L’enregistrement d’un replay ne bloque plus la capture pendant la création de la miniature, et les captures d’écran prises au même moment créent leur dossier de manière plus fiable.
- Nouvelle option pour la bordure de capture jaune de Windows, avec des informations plus claires sur l’autorisation et la prise en charge par le système. L’enregistrement fonctionne toujours lorsque Windows ne peut pas masquer la bordure.

## Manettes et commandes

- Détection des manettes et acheminement des commandes plus fiables lorsque plusieurs manettes ou sources d’entrée sont connectées.
- Le changement de source d’entrée, la reconnexion des manettes et le suivi des boutons maintenus évitent désormais les appuis manqués, les appuis en double et les commandes bloquées.
- Correction des appuis sur le bouton PS qui n’étaient pas détectés, et des répétitions tardives qui pouvaient rouvrir la surcouche juste après sa fermeture.
- Les gâchettes, les clics de sticks et les autres boutons se comportent désormais de la même façon sur toutes les sources d’entrée prises en charge, pour des affectations plus prévisibles.
- Les manettes qui utilisent GameInput continuent de fonctionner normalement, y compris l’isolation de la surcouche, lorsque Windows ne peut pas fournir la prise en charge facultative des boutons Guide/Share.
- La manette que vous modifiez dans les Paramètres reste sélectionnée lorsqu’une autre manette devient active.

## Préréglages d’affectation

- Nouvelle bibliothèque de préréglages pour les configurations de manette, dans laquelle vous pouvez créer, renommer, dupliquer, modifier et supprimer des préréglages.
- Les préréglages peuvent être attribués aux manettes et aux jeux, avec un choix de secours et une sélection automatique pour le jeu en cours.
- Vos affectations personnalisées existantes sont automatiquement transférées dans le système de préréglages, et les données d’origine sont conservées en sauvegarde.
- Le changement de préréglage tient compte des boutons maintenus et des gestes en cours, afin de ne déclencher aucune action par accident.
- L’interface distingue le préréglage attribué de celui que vous modifiez, protège les modifications non enregistrées et vous permet de créer une copie pour une seule manette.
- Avant de supprimer un préréglage en cours d’utilisation, vous devez choisir un remplaçant ou une solution de secours, et les modifications apportées aux préréglages partagés sont clairement signalées.

## Interface, paramètres et son

- GameHQ se rouvre sur la dernière page, la catégorie des Paramètres et le filtre de galerie que vous avez utilisés. La surcouche mémorise sa dernière catégorie séparément pour chaque jeu.
- La fenêtre se restaure désormais correctement sur les écrans situés à gauche ou au-dessus de votre écran principal. Une fenêtre qui s’ouvrirait entièrement hors écran est replacée sur un écran connecté.
- La fenêtre principale et la surcouche disposent chacune de leur propre mise à l’échelle, de 100 % à 200 %, conservée d’un redémarrage à l’autre. La mise en page des petites fenêtres est améliorée.
- Les sons de capture sont plus forts et plus distincts, avec leur propre réglage de volume et un aperçu. Le volume de l’interface et celui des captures peuvent monter jusqu’à 300 %.
- Modifier les paramètres de notification, de son, de bordure de capture ou de session manuelle ne vide plus votre tampon de replay.
- La galerie conserve son filtre enregistré à l’ouverture. Un filtre enregistré pour un jeu qui n’existe plus revient sans risque à une valeur par défaut. Les catégories des Paramètres restent en place si leur ordre change.
- Correction des contenus vides et des problèmes de superposition lors du changement d’échelle de l’interface ; les menus s’affichent désormais au-dessus du contenu principal.
- Les réglages d’affectation indiquent la durée d’un appui long et expliquent le fonctionnement des gestes d’appui long.

## Mises à jour, langues et diagnostic

- Les notes de mise à jour s’affichent dans la langue choisie, reviennent à l’anglais lorsqu’aucune traduction n’est disponible et restent lisibles hors ligne une fois chargées.
- Les notes de mise à jour peuvent inclure un lien facultatif vers la version publiée sur GitHub, accessible à la manette.
- Les traductions ont été complétées dans toutes les langues prises en charge pour les nouveaux messages de capture, de son, de bordure, d’appui long, de préréglage et de focus.
- Les diagnostics copiés incluent désormais les affectations actives, la sélection de préréglage, les changements de source d’entrée et l’état du focus de la surcouche, avec des identifiants de périphérique anonymisés.
- Les diagnostics de capture suivent chaque demande, de l’appui sur le bouton jusqu’au fichier enregistré, ce qui facilite la recherche des problèmes. Les problèmes de son sont signalés par un avertissement clair.

## Limitations connues

- L’isolation de la manette par rapport au jeu dépend du jeu et de la façon dont il lit la manette. Une DualSense filaire utilisant GameInput a été largement testée. Cette isolation n’est pas garantie avec XInput, Raw Input, HID direct, Steam Input ou les manettes virtuelles.
- Le changement de manette lorsque DSX est en cours d’exécution n’est que partiellement vérifié. Les configurations DSX restent sous votre contrôle ; GameHQ n’installe ni ne gère aucun pilote de manette virtuelle ou de masquage de périphérique.
- Certains jeux se mettent en pause ou réagissent lorsqu’ils perdent le focus. Windows ou une autre application de capture peut laisser visible la bordure d’enregistrement jaune.
- Lorsque les notes de version 0.7.8 ne sont pas disponibles dans votre langue, GameHQ affiche la version anglaise.
