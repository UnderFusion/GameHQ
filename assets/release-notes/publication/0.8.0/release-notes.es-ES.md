# GameHQ 0.8.0 (2026-10-26)

## Agradecimientos especiales

- Muchísimas gracias a [@galcn10](https://github.com/galcn10), creador de [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), por probar GameHQ, compartir comentarios muy valiosos y crear una gran integración con Playnite. ¡Tu apoyo ha ayudado a mejorar GameHQ 0.8.0!

## Lo más destacado

- Los clips pueden grabar el sonido propio del juego. Con la nueva fuente Audio del juego, GameHQ graba el juego en el dispositivo por el que se reproduzca (auriculares, Bluetooth, HDMI o altavoces) y deja fuera otras aplicaciones como Discord o Spotify.
- Ahora se detectan y se notifican los problemas de grabación de audio. Si no se puede grabar la fuente de audio seleccionada, el búfer de repetición se detiene y te indica el motivo y qué fuente elegir en su lugar, en vez de guardar sin avisar clips sin sonido. GameHQ nunca cambia la fuente de audio por su cuenta.
- El sonido y la imagen se mantienen sincronizados. Los clips guardados después de una pantalla de carga, una pausa o un bloqueo prolongado conservan los tiempos correctos, y el búfer de repetición se reinicia solo cuando un juego cambia de resolución.
- Las capturas de pantalla HDR ya no se quedan atascadas. Una captura de pantalla de un juego en pausa o de un menú estático en HDR se guarda al instante. Si no llega ningún fotograma, el intento falla limpiamente al cabo de unos segundos y la siguiente pulsación funciona.
- Eliminación más segura con el mando. En una confirmación de eliminación, Cruz ahora pulsa el botón resaltado y Círculo cancela. Antes, Cruz podía eliminar el archivo incluso con Cancelar resaltado.
- Eliminar protege tus originales. Al eliminar una captura creada por GameHQ, ahora se mueve a la Papelera de reciclaje, así que puedes restaurarla. Los archivos de las carpetas supervisadas nunca se eliminan.

## Novedades

- Configuración › Repetición › Fuente de audio ofrece Audio del juego, Audio del escritorio y Desactivado. Audio del juego es la opción predeterminada en Windows 11. Audio del escritorio graba todo lo que suena por la salida predeterminada de Windows, incluidas otras aplicaciones, y es la opción predeterminada en versiones anteriores de Windows.
- Audio del juego usa la captura de audio por aplicación del propio Windows y nunca toca el proceso del juego. Si habías desactivado a propósito el audio del sistema en una versión anterior, sigue desactivado.
- Actualizaciones beta opcionales: un nuevo interruptor Incluir versiones beta en Configuración › General y en Acerca de permite que el actualizador ofrezca versiones beta. Está desactivado de forma predeterminada, así que las instalaciones estables solo reciben actualizaciones estables.
- Cuando GameHQ se cierra inesperadamente, intenta guardar un pequeño volcado de memoria en su carpeta de registros para ayudar a diagnosticar el problema. No se sube nada. Algunos cierres inesperados, como aquellos en los que Windows finaliza el programa de inmediato, todavía pueden cerrar GameHQ sin generar un volcado.
- El registro anota más detalles sobre las capturas de pantalla guardadas y los fallos de inicio, así que un único archivo de registro basta más a menudo para entender un informe de problema.

## Correcciones y mejoras

- Los clips ya no se quedan sin sonido por culpa de un antiguo ajuste interno que mantenía desactivado el audio del escritorio de forma predeterminada.
- El audio en cualquier formato de reproducción de Windows se convierte correctamente en lugar de perderse la pista de audio.
- Los momentos de silencio se graban como silencio, para que el sonido posterior a un tramo silencioso siga alineado con la imagen.
- La grabación de Audio del juego se reinicia de forma más fiable cuando un juego se reinicia o cambias de juego.
- Las partes del búfer de repetición que no se pudieron guardar o que no se pueden volver a leer se descartan, para que no puedan estropear un clip guardado.
- Las notificaciones del propio GameHQ, como el aviso de que se ha guardado una captura de pantalla, ya no aparecen en tus capturas de pantalla ni en las grabaciones y retransmisiones de toda la pantalla.
- Una captura de pantalla sustituida por otra imagen con el mismo nombre de archivo obtiene una miniatura nueva en el siguiente análisis de la biblioteca.
- Las carpetas de capturas se analizan en segundo plano, así que GameHQ sigue respondiendo con colecciones grandes.
- Los navegadores, los editores y otras ventanas que no son juegos ya no aparecen como juegos en tu biblioteca.
- Durante una eliminación múltiple, la selección se mantiene fija mientras la confirmación está abierta, para que las acciones del mando en segundo plano no puedan cambiar qué archivos se eliminan.
- El cuadro de diálogo de eliminación de la superposición recibe la entrada del mando mientras está abierto y se cierra cuando se oculta la superposición.
- La superposición omite las ventanas que han dejado de responder en lugar de esperarlas.
- Configuración ya no vuelve al principio cuando te desplazas con el ratón después de volver a GameHQ.
- Elegir Valores predeterminados integrados o quitar un preajuste en el editor de asignación del mando ya no bloquea la selección de preajustes.

## Mandos y Playnite

- Los atajos del mando siguen funcionando en juegos iniciados fuera de Steam, como un juego local iniciado desde Playnite. Cuando Windows deja de entregar a GameHQ un DualSense conectado mientras el juego tiene el foco, GameHQ lee el mando directamente, en modo de solo lectura, y vuelve a la entrada normal en cuanto Windows vuelve a entregarlo.
- Los atajos y las repeticiones al mantener pulsado un botón siempre terminan al soltar esa misma pulsación, incluso cuando la liberación se notifica a través de GameInput o el mando se desconecta mientras se mantiene pulsado un botón.
- Se limpian los mandos que se han desconectado definitivamente, la comprobación de HidHide ya no ralentiza la interfaz y los dispositivos GameInput solo se vuelven a comprobar cuando algo cambia.
- Los juegos de Playnite se siguen reconociendo aunque Playnite no pueda identificar el proceso del juego.
- Experiencia de pantalla completa de Xbox en Windows: cuando la interfaz de Xbox está en primer plano, GameHQ usa el juego iniciado desde Playnite para las capturas de pantalla y los clips. GameHQ Playnite Integration 0.4.13, disponible por separado como versión preliminar, también inicia GameHQ cuando el modo Xbox lo retrasa al iniciar sesión.

## Estabilidad y fiabilidad

- La limpieza de grabaciones sin terminar solo elimina los archivos temporales que ha creado el propio GameHQ.
- Las carpetas de capturas de los juegos siempre permanecen dentro de tu carpeta de capturas.
- El inicio ya no falla cuando la biblioteca contiene rutas de carpeta duplicadas; en su lugar, las entradas se combinan.
- Si GameHQ no puede iniciarse, explica el motivo en tu idioma y lo guarda en el registro.
- Importar un perfil portátil en un GameHQ instalado ahora funciona aunque la copia instalada ya se haya iniciado.
- Seguridad reforzada: carga más segura de DLL, comprobaciones más estrictas de las conexiones de integración locales y gestión más segura de los archivos de actualización.
- El actualizador gestiona mejor un primer inicio lento y mantiene intactos sus datos si se interrumpe.
- El uso compartido con Telegram se cierra de forma más segura.

## Instalación y actualización

- Actualizar desde 0.7.9: ejecuta el instalador de la 0.8.0 sobre tu instalación actual.
- Instalador: instala para el usuario actual y sustituye una 0.7.9 instalada o una beta anterior.
- Versión portátil: extrae el ZIP en una carpeta nueva y ejecuta GameHQ.exe.
- Si has probado la 0.8.0 Beta 2, 3 o 4, instala la 0.8.0 con el instalador o con un ZIP portátil nuevo, porque esas betas no se ofrecieron a través del actualizador.

## Limitaciones conocidas

- Audio del juego requiere Windows 11. En versiones anteriores de Windows, GameHQ usa Audio del escritorio, que también graba otras aplicaciones que suenan por la salida predeterminada.
- Un juego que toma el control exclusivo de un mando puede seguir impidiendo los atajos en segundo plano.
- Los testers aún no han confirmado la compatibilidad con la experiencia de pantalla completa de Xbox de Windows en un PC que la utilice.
- Los binarios de Windows no están firmados con Authenticode; Windows SmartScreen puede mostrar una advertencia de Editor desconocido.
